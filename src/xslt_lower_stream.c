/*
 * Streamed evaluation in XSLT: the lowering (src/xslt_lower_internal.h,
 * "Streamed evaluation"; XSLT 4.0, 18.1 and 19; D219, D223, after D212).
 *
 * A streamed body (glossary `stream_point`, `stream_window`): the
 * sequence constructor of xsl:source-document with streamable="yes", and
 * of the xsl:for-each instructions in it that select streamed nodes. Its
 * instructions (inside literal result elements, which only wrap their
 * content) are each either motionless in the focus of the streamed node
 * (XSLT 4.0, 19: they read its name, attributes and ancestors, never its
 * content; the sweep is 7.6a's, src/xslt_streamability_internal.h) or a
 * stream point, of which a streamed body has at most one. A stream point
 * is
 *
 * - an xsl:for-each whose select is a downward path from the focus,
 *
 *       step ("/" step)*            step: child::T, T, descendant::T
 *       "//" T at the start, or as the last step (descendant::T)
 *
 *   each step with at most one predicate that is motionless, the
 *   descendant step last, and whose body is at most consuming; or an
 *   xsl:apply-templates with such a select whose mode's template rules are
 *   guaranteed-streamable (6.6.4), applied to each node in its full window
 *   with its position in the path (APPLY_TEMPLATES of the STREAM's focus;
 *   a template rule reading fn:last keeps it from streaming). It becomes
 *   a chain of STREAMs, one per step, the body of each the STREAM of the
 *   next (src/ir_internal.h; the path's position in a counter slot the
 *   chain shares). A stream point whose own body is streamed gets shallow
 *   windows: the selected element's content is read by the body's stream
 *   point as it passes; otherwise each selected node's window holds its
 *   subtree (the body consumes it, 19.8.1), built while its events pass;
 *
 * - or a streamed path (glossary `streamed_path`, D223): such a path,
 *   perhaps with a last attribute step ("E/@a" is "E ! @a"), inside the
 *   select of an xsl:value-of, xsl:copy-of, xsl:sequence, xsl:for-each,
 *   xsl:apply-templates (whose mode's template rules are all
 *   guaranteed-streamable, 6.6.4), xsl:iterate or xsl:for-each-group that
 *   is consuming and grounded as a whole, the rest of the expression
 *   motionless around it (operators, conditionals and function calls; the
 *   right operand of a simple map or filter on it reads its nodes). The
 *   path's node is lowered in place through a lowering override
 *   (glossary `lowering_override`, src/xpath_lower_internal.h) to a chain
 *   whose body grounds what it selects (19.4): the node in a full window;
 *   its atomized value for a function that atomizes it
 *   (fn:distinct-values and others); for fn:sum, fn:min and fn:max of one
 *   argument, the aggregate folded node by node in the chain's fold slot;
 *   for fn:count, fn:exists and fn:empty, nothing but the chain's counter,
 *   in shallow windows. Aggregates of a large document so take no memory
 *   per node. Instructions that run on the selected items
 *   (a body, sort keys, template rules) run on their full windows, after
 *   the selection.
 *
 * xsl:source-document becomes STREAM with OPENS_DOCUMENT over its href.
 * Everything else is lowered as before, unstreamed: an xsl:source-document
 * whose body has another shape reads the whole document (fn:doc), which
 * gives the same results (19.10). Accumulators are computed as the
 * document's events pass (D231; end rules with capture="yes" read
 * captured subtrees, packet 7.6g). An xsl:merge some of whose sources are
 * streamable becomes MERGE (src/ir_internal.h, packet 7.6g): each such
 * source's select, a path of child steps, is a chain of STREAMs its
 * cursors read (sxml_xslt_lowering_add_merge_chain). Not streamed yet:
 * validation and type on xsl:source-document and xsl:merge-source, merge
 * sources with accumulators or predicates, and bodies that read
 * fn:has-children, fn:path or fn:last (motionless by 19.8.9, but they
 * need what a window lacks).
 *
 * Golden file: src/xslt_lower_control.c (xsl:for-each as a procedure of
 * the lowering machine; xsl:iterate's closure built in steps).
 */
#include "xslt_lower_internal.h"

#include "memory_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"
#include "xslt_accumulator_internal.h"
#include "xslt_streamability_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The most steps of a streamed path. */
#define STREAM_MAXIMUM_STEP_COUNT 16U

/* The most prongs of a streamed xsl:fork (D231). */
#define STREAM_MAXIMUM_PRONG_COUNT 32U

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* -------------------------------------------------------------- Plans */

typedef enum stream_point_kind {
    /* An xsl:for-each whose body runs in each selected node's window
       (src/xslt_lower_control.c lowers it here). */
    SXML_STREAM_POINT_FOR_EACH,
    /* An xsl:apply-templates whose template rules are applied to each node
       it selects, in its full window, as it passes (D223). */
    SXML_STREAM_POINT_APPLY,
    /* A streamed path (glossary `streamed_path`): a path in the select
       expression of an instruction, lowered in place through a lowering
       override (src/xpath_lower_internal.h). */
    SXML_STREAM_POINT_PATH,
    /* A prong of an xsl:fork whose focus is the document node of a
       streamed xsl:source-document (glossary `streamed_prong`, D231): an
       xsl:sequence streamed in a pass of its own over the document. */
    SXML_STREAM_POINT_PRONG,
} stream_point_kind;

/* What a streamed path gives for each node it selects (its grounding,
   XSLT 4.0, 19.4: the rest of the expression reads grounded values). */
typedef enum stream_grounding {
    /* The node, in a full window (its subtree). */
    SXML_STREAM_GROUNDING_NODES,
    /* fn:data of the node: the operand of a function that atomizes it
       (sum, avg, min, max, distinct-values, data, string-join). */
    SXML_STREAM_GROUNDING_ATOMIZED,
    /* One xs:boolean per node: the operand of fn:count, fn:exists or
       fn:empty, which read only how many there are; no content is read. */
    SXML_STREAM_GROUNDING_COUNTED,
    /* The aggregate of fn:sum, fn:min or fn:max (one argument) of the
       atomized values, folded one node at a time in the chain's fold slot
       (F&O 4.0, 14.4: each is a fold of its pairwise form). */
    SXML_STREAM_GROUNDING_FOLDED,
    /* fn:avg of one argument: the sum folded as for FOLDED, divided by
       the chain's counter after it (D228). */
    SXML_STREAM_GROUNDING_AVERAGED,
} stream_grounding;

/* A stream point found in a streamed body. */
typedef struct stream_point {
    stream_point_kind kind;
    sxml_xslt_ast_node_index node;
    /* Its body is streamed (FOR_EACH), or its nodes need no content
       (PATH): shallow windows for its items. */
    bool is_shallow;
    /* PATH: the path's AST (an expression of the stylesheet AST), its
       top node and grounding. */
    const sxml_ast *ast;
    sxml_ast_node_index path;
    stream_grounding grounding;
    /* PATH: the node its override lowers: the path, or the fn:count,
       fn:exists or fn:empty call of a counted path. */
    sxml_ast_node_index lowered;
    /* PATH: the xsl:source-document whose body it is in, and whether its
       override has lowered it (each must have, or the body would read a
       shallow window as a tree). */
    sxml_xslt_ast_node_index document;
    bool has_lowered;
} stream_point;

/* A body to plan: the sequence constructor of node; point_index + 1 is
   its stream point (0 for the xsl:source-document's own body). */
typedef struct stream_body {
    sxml_xslt_ast_node_index node;
    size_t point_number;
    /* Its focus is a document node (the document of xsl:source-document,
       a template rule matching "/"); an element otherwise. */
    bool is_document_focus;
    /* The body is node itself, not its sequence constructor (a prong). */
    bool is_instruction;
    /* The document can be read again for an xsl:fork's prongs: the
       document of xsl:source-document, whose href the lowering has. */
    bool allows_passes;
} stream_body;

struct sxml_xslt_stream_plan {
    sxml_allocator allocator;
    stream_point *points;
    size_t point_count;
    size_t point_capacity;
    /* One lowering override per PATH point (lowering->lowerer's view). */
    sxml_xpath_lowering_override *overrides;
    size_t override_count;
    size_t override_capacity;
    /* The href of the streamed xsl:source-document being lowered, for the
       passes of its prongs (D231). */
    sxml_ir_atom document_href;
};

sxml_status sxml_xslt_create_stream_plan(sxml_allocator allocator, sxml_xslt_stream_plan **plan) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_xslt_stream_plan),
                                  .alignment = alignof(sxml_xslt_stream_plan)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_stream_plan *new_plan = memory;
    *new_plan = (sxml_xslt_stream_plan){.allocator = allocator,
                                        .points = NULL,
                                        .point_count = 0,
                                        .point_capacity = 0,
                                        .overrides = NULL,
                                        .override_count = 0,
                                        .override_capacity = 0,
                                        .document_href = {.atom_kind = SXML_IR_ATOM_KIND_NONE}};
    *plan = new_plan;
    return SXML_STATUS_OK;
}

void sxml_xslt_release_stream_plan(sxml_xslt_stream_plan *plan) {
    if (plan == NULL) {
        return;
    }
    sxml_allocator_release(plan->allocator, plan->points);
    sxml_allocator_release(plan->allocator, plan->overrides);
    sxml_allocator_release(plan->allocator, plan);
}

/* Makes room for one more element in an array of the plan. */
typedef struct plan_array {
    void **elements;
    size_t *capacity;
    size_t count;
    size_t element_size;
    size_t element_alignment;
} plan_array;

static sxml_status grow_plan_array(const sxml_xslt_stream_plan *plan, plan_array array) {
    if (array.count < *array.capacity) {
        return SXML_STATUS_OK;
    }
    const size_t capacity = *array.capacity == 0 ? 8U : *array.capacity * 2U;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        plan->allocator,
        (sxml_array_allocation_request){.element_count = capacity,
                                        .element_size = array.element_size,
                                        .element_alignment = array.element_alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (array.count > 0) {
        memcpy(memory, *array.elements, array.count * array.element_size);
    }
    sxml_allocator_release(plan->allocator, *array.elements);
    *array.elements = memory;
    *array.capacity = capacity;
    return SXML_STATUS_OK;
}

static sxml_status add_point(sxml_xslt_stream_plan *plan, stream_point point) {
    void *points = plan->points;
    const sxml_status status =
        grow_plan_array(plan, (plan_array){.elements = &points,
                                           .capacity = &plan->point_capacity,
                                           .count = plan->point_count,
                                           .element_size = sizeof(stream_point),
                                           .element_alignment = alignof(stream_point)});
    plan->points = points;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    plan->points[plan->point_count] = point;
    plan->point_count += 1;
    return SXML_STATUS_OK;
}

/* Gives the lowerer the plan's overrides (after they changed). */
static void publish_overrides(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_stream_plan *plan = lowering->stream_plan;
    lowering->lowerer->overrides = (sxml_xpath_lowering_override_view){
        .overrides = plan->overrides, .override_count = plan->override_count};
}

static const sxml_xpath_lowering_procedure streamed_path_procedure;

/* Adds a PATH point and its override. */
static sxml_status add_path_point(const sxml_xslt_lowering *lowering, stream_point point) {
    sxml_xslt_stream_plan *plan = lowering->stream_plan;
    void *overrides = plan->overrides;
    sxml_status status = grow_plan_array(
        plan, (plan_array){.elements = &overrides,
                           .capacity = &plan->override_capacity,
                           .count = plan->override_count,
                           .element_size = sizeof(sxml_xpath_lowering_override),
                           .element_alignment = alignof(sxml_xpath_lowering_override)});
    plan->overrides = overrides;
    if (status == SXML_STATUS_OK) {
        status = add_point(plan, point);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    plan->overrides[plan->override_count] =
        (sxml_xpath_lowering_override){.ast = point.ast,
                                       .node = point.lowered,
                                       .procedure = &streamed_path_procedure,
                                       .state = lowering,
                                       .number = plan->point_count - 1U};
    plan->override_count += 1;
    publish_overrides(lowering);
    return SXML_STATUS_OK;
}

/* Forgets the points from first_point on (a body not streamed after
   all), and their overrides. */
static void drop_points(const sxml_xslt_lowering *lowering, size_t first_point) {
    sxml_xslt_stream_plan *plan = lowering->stream_plan;
    while (plan->override_count > 0 &&
           plan->overrides[plan->override_count - 1U].number >= first_point) {
        plan->override_count -= 1;
    }
    plan->point_count = first_point;
    publish_overrides(lowering);
}

static const stream_point *find_point(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node) {
    const sxml_xslt_stream_plan *plan = lowering->stream_plan;
    if (plan == NULL) {
        return NULL;
    }
    for (size_t point_index = 0; point_index < plan->point_count; point_index += 1) {
        if (plan->points[point_index].kind != SXML_STREAM_POINT_PATH &&
            plan->points[point_index].node.value == node.value) {
            return &plan->points[point_index];
        }
    }
    return NULL;
}

bool sxml_xslt_is_stream_point(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    return find_point(lowering, node) != NULL;
}

/* ------------------------------------------------------------ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static const sxml_xslt_ast_attribute *attribute_of(const sxml_xslt_lowering *lowering,
                                                   sxml_xslt_ast_node_index node,
                                                   sxml_xslt_ast_attribute_kind kind) {
    return sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(lowering->ast,
                                     (sxml_xslt_ast_attribute_query){.node = node, .kind = kind}));
}

static bool has_attribute(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                          sxml_xslt_ast_attribute_kind kind) {
    return attribute_of(lowering, node, kind)->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
}

/* The expression of an attribute or text node value; NULL for others. */
static const sxml_xslt_ast_expression *expression_of_value(const sxml_xslt_lowering *lowering,
                                                           sxml_xslt_ast_value value) {
    if (value.kind != SXML_XSLT_AST_VALUE_KIND_EXPRESSION &&
        value.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE &&
        value.kind != SXML_XSLT_AST_VALUE_KIND_PATTERN) {
        return NULL;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, value.payload.expression);
    return expression->ast != NULL ? expression : NULL;
}

/* The local name of a function call or named function reference of ast
   at index; an empty view for other nodes. */
static sxml_byte_view called_name(const sxml_ast *ast, sxml_ast_node_index index) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, index);
    if (node->kind != SXML_AST_KIND_FUNCTION_CALL &&
        node->kind != SXML_AST_KIND_NAMED_FUNCTION_REFERENCE) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    const sxml_ast_node_index name = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = index, .field = SXML_AST_FIELD_NAME});
    return sxml_ast_get_text(ast, sxml_ast_get_node(ast, name)->payload.name.local_name);
}

static bool is_named(sxml_byte_view local_name, const char *name) {
    const size_t length = strlen(name);
    return local_name.byte_count == length &&
           (length == 0 || memcmp(local_name.bytes, name, length) == 0);
}

/* Some names of functions, by local name. */
typedef struct function_names {
    const char *const *names;
    size_t name_count;
} function_names;

/* Whether ast calls one of names (by local name: a guess that only ever
   keeps a body from streaming). */
static bool calls_named_function(const sxml_ast *ast, function_names names) {
    for (uint32_t index = 1; index < ast->node_count; index += 1) {
        const sxml_byte_view local_name = called_name(ast, (sxml_ast_node_index){.value = index});
        for (size_t name_index = 0; local_name.byte_count > 0 && name_index < names.name_count;
             name_index += 1) {
            if (is_named(local_name, names.names[name_index])) {
                return true;
            }
        }
    }
    return false;
}

/* The functions a stream window cannot answer (fn:has-children, fn:path;
   fn:last and fn:function-lookup, which read the size of the focus a
   streamed selection has not). */
static const char *const window_function_names[] = {"has-children", "path", "last",
                                                    "function-lookup"};

static bool calls_window_function(const sxml_ast *ast) {
    return calls_named_function(
        ast, (function_names){.names = window_function_names,
                              .name_count = sizeof(window_function_names) /
                                            sizeof(window_function_names[0])});
}

/* An XSLT subtree to search (root and its descendants) for calls of
   names, and also, with includes_calls, for template and attribute set
   calls (which pass the focus size on). */
typedef struct subtree_search {
    sxml_xslt_ast_node_index root;
    function_names names;
    bool includes_calls;
} subtree_search;

/* Whether the subtree of search has such a call. A walk with an explicit
   stack (D71). */
static sxml_status subtree_calls(const sxml_xslt_lowering *lowering, subtree_search search,
                                 bool *calls) {
    const sxml_xslt_ast_node_index root = search.root;
    *calls = false;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    size_t capacity = 64;
    void *memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = capacity,
                                              .element_size = sizeof(sxml_xslt_ast_node_index),
                                              .element_alignment =
                                                  alignof(sxml_xslt_ast_node_index)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_ast_node_index *stack = memory;
    size_t count = 0;
    stack[count] = root;
    count += 1;
    while (count > 0 && !*calls) {
        count -= 1;
        const sxml_xslt_ast_node_index node = stack[count];
        const sxml_xslt_ast_node *entry = node_of(lowering, node);
        const sxml_xslt_ast_expression *content = expression_of_value(lowering, entry->content);
        *calls = (search.includes_calls && entry->kind == SXML_XSLT_AST_KIND_CALL_TEMPLATE) ||
                 (content != NULL && calls_named_function(content->ast, search.names));
        for (uint32_t offset = 0; !*calls && offset < entry->attributes.attribute_count;
             offset += 1) {
            const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
                lowering->ast, (sxml_xslt_ast_attribute_index){
                                   .value = entry->attributes.first_attribute_index + offset});
            const sxml_xslt_ast_expression *expression =
                expression_of_value(lowering, attribute->value);
            *calls = (search.includes_calls &&
                      attribute->kind == SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS) ||
                     (expression != NULL && calls_named_function(expression->ast, search.names));
        }
        for (sxml_xslt_ast_node_index child = entry->first_child; child.value != 0;
             child = node_of(lowering, child)->next_sibling) {
            if (count == capacity) {
                void *grown = NULL;
                status = sxml_xpath_lowerer_allocate_scratch(
                    lowerer,
                    (sxml_xpath_lowering_scratch_request){
                        .element_count = capacity * 2U,
                        .element_size = sizeof(sxml_xslt_ast_node_index),
                        .element_alignment = alignof(sxml_xslt_ast_node_index)},
                    &grown);
                if (status != SXML_STATUS_OK) {
                    sxml_xpath_lowerer_release_scratch(lowerer, stack);
                    return status;
                }
                memcpy(grown, stack, count * sizeof(sxml_xslt_ast_node_index));
                sxml_xpath_lowerer_release_scratch(lowerer, stack);
                stack = grown;
                capacity *= 2U;
            }
            stack[count] = child;
            count += 1;
        }
    }
    sxml_xpath_lowerer_release_scratch(lowerer, stack);
    return SXML_STATUS_OK;
}

/* Whether the subtree of root calls a function a window cannot answer,
   a template or an attribute set. */
static sxml_status subtree_calls_window_function(const sxml_xslt_lowering *lowering,
                                                 sxml_xslt_ast_node_index root, bool *calls) {
    return subtree_calls(
        lowering,
        (subtree_search){.root = root,
                         .names = {.names = window_function_names,
                                   .name_count = sizeof(window_function_names) /
                                                 sizeof(window_function_names[0])},
                         .includes_calls = true},
        calls);
}

/* --------------------------------------------------------------- Sweeps */

/* What a construct asked about is. */
typedef struct sweep_question {
    sxml_xslt_ast_node_index instruction;
    bool assesses_content;
    const sxml_xslt_ast_expression *expression;
    sxml_ast_node_index expression_node;
    bool is_document_focus;
    /* False for a predicate, whose streamed nodes are only tested. */
    bool requires_grounded;
    /* The instruction comes after the stream point of its body, which
       has read the node's content (19.8.9.1: fn:accumulator-after is then
       motionless, D231). */
    bool follows_consuming;
} sweep_question;

static sxml_status sweep_of(const sxml_xslt_lowering *lowering, sweep_question question,
                            sxml_xslt_stream_sweep *sweep) {
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    return sxml_xslt_assess_sweep(
        &(sxml_xslt_sweep_query){
            .ast = lowering->ast,
            .allocator = lowerer->allocator,
            .name_pool = lowerer->name_pool,
            .instruction = question.instruction,
            .assesses_content = question.assesses_content,
            .expression_ast = question.expression != NULL ? question.expression->ast : NULL,
            .expression_node = question.expression_node,
            .static_context = question.expression != NULL
                                  ? question.expression->static_context
                                  : (sxml_xslt_ast_static_context_index){.value = 0},
            .is_document_focus = question.is_document_focus,
            .requires_grounded = question.requires_grounded,
            .follows_consuming = question.follows_consuming},
        sweep);
}

static sxml_status is_motionless(const sxml_xslt_lowering *lowering, sweep_question question,
                                 bool *motionless) {
    sxml_xslt_stream_sweep sweep = SXML_XSLT_STREAM_SWEEP_UNCERTAIN;
    const sxml_status status = sweep_of(lowering, question, &sweep);
    *motionless = status == SXML_STATUS_OK && sweep == SXML_XSLT_STREAM_SWEEP_MOTIONLESS;
    return status;
}

/* ------------------------------------------------------- Streamed paths */

/* One step of a streamed path. */
typedef struct stream_step {
    sxml_ast_node_index step;
    sxml_axis axis;
    /* The predicate's expression; 0 for none. */
    sxml_ast_node_index predicate;
} stream_step;

typedef struct stream_path {
    stream_step steps[STREAM_MAXIMUM_STEP_COUNT];
    uint32_t step_count;
    /* An attribute step after the last one (XPath 4.0, 4.6.4); 0 for
       none: "E/@a" is "E ! @a", the attributes of each element E selects,
       which are in its shallow window. */
    sxml_ast_node_index attribute_step;
} stream_path;

/* The step's axis as written, CHILD for none; false for other axes and
   attribute tests. */
static bool step_axis(const sxml_ast *ast, sxml_ast_node_index step, sxml_axis *axis) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, step);
    if ((node->flags & (uint32_t)SXML_AST_FLAG_ABBREVIATED) != 0) {
        return false;
    }
    const sxml_ast_node_index test = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = step, .field = SXML_AST_FIELD_TEST});
    if (test.value == 0) {
        return false;
    }
    /* Tests of the nodes the child and descendant axes reach. */
    const sxml_ast_kind test_kind = sxml_ast_get_node(ast, test)->kind;
    if (test_kind != SXML_AST_KIND_NCNAME && test_kind != SXML_AST_KIND_PREFIXED_NAME &&
        test_kind != SXML_AST_KIND_URI_QUALIFIED_NAME && test_kind != SXML_AST_KIND_WILDCARD &&
        test_kind != SXML_AST_KIND_ELEMENT_NODE_TYPE && test_kind != SXML_AST_KIND_TEXT_NODE_TYPE &&
        test_kind != SXML_AST_KIND_COMMENT_NODE_TYPE &&
        test_kind != SXML_AST_KIND_PROCESSING_INSTRUCTION_NODE_TYPE &&
        test_kind != SXML_AST_KIND_ANY_XNODE_TYPE) {
        return false;
    }
    const sxml_ast_axis written = node->detail.axis;
    if (written == SXML_AST_AXIS_NONE || written == SXML_AST_AXIS_CHILD) {
        *axis = SXML_AXIS_CHILD;
        return true;
    }
    if (written == SXML_AST_AXIS_DESCENDANT) {
        *axis = SXML_AXIS_DESCENDANT;
        return true;
    }
    return false;
}

/* A predicate whose value cannot be a number (a comparison, a boolean
   operator, an attribute test): what "//" may turn into descendant::
   (XPath 4.0, 4.6.3; D212). */
static bool is_plainly_boolean(const sxml_ast *ast, sxml_ast_node_index expression) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, expression);
    return node->kind == SXML_AST_KIND_COMPARISON_EXPRESSION ||
           node->kind == SXML_AST_KIND_AND_EXPRESSION || node->kind == SXML_AST_KIND_OR_EXPRESSION;
}

/* Reads the step into path, after the separator before it (NONE, SLASH
   or DOUBLE_SLASH); false when it is not a streamed step. */
typedef struct step_reading {
    const sxml_ast *ast;
    sxml_ast_node_index step;
    sxml_ast_operator separator;
} step_reading;

static bool read_step(step_reading reading, stream_path *path) {
    const sxml_ast *ast = reading.ast;
    if (sxml_ast_get_node(ast, reading.step)->kind != SXML_AST_KIND_AXIS_STEP ||
        path->step_count == STREAM_MAXIMUM_STEP_COUNT) {
        return false;
    }
    stream_step *step = &path->steps[path->step_count];
    *step = (stream_step){.step = reading.step, .axis = SXML_AXIS_CHILD, .predicate = {0}};
    if (!step_axis(ast, reading.step, &step->axis)) {
        return false;
    }
    sxml_ast_node_index predicate = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = reading.step, .field = SXML_AST_FIELD_PREDICATE});
    if (predicate.value != 0) {
        if (sxml_ast_find_next_with_same_field(ast, predicate).value != 0) {
            return false;
        }
        step->predicate = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = predicate, .field = SXML_AST_FIELD_EXPRESSION});
    }
    if (reading.separator == SXML_AST_OPERATOR_DOUBLE_SLASH) {
        /* E//T is E/descendant::T for a child step whose predicate cannot
           be positional. */
        if (step->axis != SXML_AXIS_CHILD ||
            (step->predicate.value != 0 && !is_plainly_boolean(ast, step->predicate))) {
            return false;
        }
        step->axis = SXML_AXIS_DESCENDANT;
    }
    path->step_count += 1;
    return true;
}

/* Whether step is an attribute step: "@T" or "attribute::T". */
static bool is_attribute_step(const sxml_ast *ast, sxml_ast_node_index step) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, step);
    return node->kind == SXML_AST_KIND_AXIS_STEP && node->detail.axis == SXML_AST_AXIS_ATTRIBUTE;
}

/* The steps of a streamed path (see the top of the file), or false. At
   the document node, "/" and "//" at the start mean the focus. */
typedef struct path_reading {
    const sxml_ast *ast;
    /* The path's node; 0 for the expression of the AST's root. */
    sxml_ast_node_index node;
    bool is_document_focus;
    /* A last attribute step is allowed (a streamed path). */
    bool allows_attribute;
} path_reading;

static bool read_path(path_reading reading, stream_path *path) {
    const sxml_ast *ast = reading.ast;
    path->step_count = 0;
    path->attribute_step = (sxml_ast_node_index){0};
    sxml_ast_node_index node =
        reading.node.value != 0
            ? reading.node
            : sxml_ast_find_child(
                  ast, (sxml_ast_child_query){.parent = ast->root, .field = SXML_AST_FIELD_BODY});
    /* The right-hand steps and their separators, right to left. */
    sxml_ast_node_index right_steps[STREAM_MAXIMUM_STEP_COUNT];
    sxml_ast_operator separators[STREAM_MAXIMUM_STEP_COUNT];
    uint32_t right_count = 0;
    sxml_ast_operator first_separator = SXML_AST_OPERATOR_NONE;
    sxml_ast_node_index first = {0};
    for (;;) {
        const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
        if (entry->kind == SXML_AST_KIND_RELATIVE_PATH_EXPRESSION) {
            if (right_count == STREAM_MAXIMUM_STEP_COUNT) {
                return false;
            }
            right_steps[right_count] = sxml_ast_find_child(
                ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_RIGHT});
            separators[right_count] = entry->detail.operator;
            if (right_count == 0 && reading.allows_attribute &&
                entry->detail.operator == SXML_AST_OPERATOR_SLASH &&
                is_attribute_step(ast, right_steps[0])) {
                /* The last step selects attributes: not a step of the
                   chain. */
                path->attribute_step = right_steps[0];
                node = sxml_ast_find_child(
                    ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_LEFT});
                continue;
            }
            right_count += 1;
            node = sxml_ast_find_child(
                ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_LEFT});
            continue;
        }
        if (entry->kind == SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION && reading.is_document_focus &&
            first_separator == SXML_AST_OPERATOR_NONE) {
            first_separator = entry->detail.operator;
            node = sxml_ast_find_child(
                ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_PATH});
            if (node.value == 0) {
                return false;
            }
            continue;
        }
        if (entry->kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE &&
            first_separator == SXML_AST_OPERATOR_NONE && right_count > 0 &&
            separators[right_count - 1U] == SXML_AST_OPERATOR_SLASH) {
            /* "./E" is E. */
            right_count -= 1U;
            first = right_steps[right_count];
            break;
        }
        if (entry->kind != SXML_AST_KIND_AXIS_STEP) {
            return false;
        }
        first = node;
        break;
    }
    if (!read_step((step_reading){.ast = ast, .step = first, .separator = first_separator}, path)) {
        return false;
    }
    for (uint32_t index = right_count; index > 0; index -= 1U) {
        if (!read_step((step_reading){.ast = ast,
                                      .step = right_steps[index - 1U],
                                      .separator = separators[index - 1U]},
                       path)) {
            return false;
        }
    }
    /* A descendant step selects nodes inside nodes it selects: last only. */
    for (uint32_t index = 0; index + 1U < path->step_count; index += 1U) {
        if (path->steps[index].axis == SXML_AXIS_DESCENDANT) {
            return false;
        }
    }
    return true;
}

/* The select expression of an xsl:for-each. */
static const sxml_xslt_ast_expression *select_of(const sxml_xslt_lowering *lowering,
                                                 sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *select =
        attribute_of(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    return select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION
               ? expression_of_value(lowering, select->value)
               : NULL;
}

/* Whether node is an xsl:for-each that can be a stream point in a body
   whose focus is a streamed document node or element. */
typedef struct point_question {
    sxml_xslt_ast_node_index node;
    bool is_document_focus;
    /* After the body's stream point (D231). */
    bool follows_point;
} point_question;

static sxml_status is_eligible_point(const sxml_xslt_lowering *lowering, point_question question,
                                     bool *is_eligible) {
    *is_eligible = false;
    const sxml_xslt_ast_node_index node = question.node;
    if (node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_FOR_EACH ||
        sxml_xslt_lowering_has_sort(lowering, node) ||
        has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SEPARATOR) ||
        sxml_xslt_lowering_has_xslt_1_behaviour(lowering, node)) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *select = select_of(lowering, node);
    stream_path path;
    if (select == NULL ||
        !read_path(
            (path_reading){.ast = select->ast, .is_document_focus = question.is_document_focus},
            &path)) {
        return SXML_STATUS_OK;
    }
    for (uint32_t index = 0; index < path.step_count; index += 1U) {
        if (path.steps[index].predicate.value == 0) {
            continue;
        }
        bool motionless = false;
        const sxml_status status =
            is_motionless(lowering,
                          (sweep_question){.instruction = {0},
                                           .assesses_content = false,
                                           .expression = select,
                                           .expression_node = path.steps[index].predicate,
                                           .is_document_focus = false,
                                           .requires_grounded = false},
                          &motionless);
        if (status != SXML_STATUS_OK || !motionless) {
            return status;
        }
    }
    if (calls_window_function(select->ast)) {
        return SXML_STATUS_OK;
    }
    sxml_xslt_stream_sweep body = SXML_XSLT_STREAM_SWEEP_UNCERTAIN;
    sxml_status status = sweep_of(lowering,
                                  (sweep_question){.instruction = node,
                                                   .assesses_content = true,
                                                   .expression = NULL,
                                                   .expression_node = no_ast_node,
                                                   .is_document_focus = false,
                                                   .requires_grounded = true},
                                  &body);
    if (status != SXML_STATUS_OK ||
        (body != SXML_XSLT_STREAM_SWEEP_MOTIONLESS && body != SXML_XSLT_STREAM_SWEEP_CONSUMING)) {
        return status;
    }
    bool calls = false;
    status = subtree_calls_window_function(lowering, node, &calls);
    /* fn:last is free-ranging (19.8.9.14); fn:path and fn:has-children
       read siblings and children a window may lack. */
    *is_eligible = status == SXML_STATUS_OK && !calls;
    return status;
}

/* Whether one instruction of a streamed body reads nothing of the
   streamed node's content. */
static sxml_status is_motionless_instruction(const sxml_xslt_lowering *lowering,
                                             point_question question, bool *motionless) {
    const sxml_xslt_ast_node *entry = node_of(lowering, question.node);
    *motionless = false;
    if (entry->kind == SXML_XSLT_AST_KIND_TEXT_NODE) {
        const sxml_xslt_ast_expression *text = expression_of_value(lowering, entry->content);
        if (text == NULL) {
            *motionless = true;
            return SXML_STATUS_OK;
        }
        if (calls_window_function(text->ast)) {
            return SXML_STATUS_OK;
        }
        return is_motionless(lowering,
                             (sweep_question){.instruction = {0},
                                              .assesses_content = false,
                                              .expression = text,
                                              .expression_node = text->ast->root,
                                              .is_document_focus = question.is_document_focus,
                                              .requires_grounded = true,
                                              .follows_consuming = question.follows_point},
                             motionless);
    }
    sxml_status status =
        is_motionless(lowering,
                      (sweep_question){.instruction = question.node,
                                       .assesses_content = false,
                                       .expression = NULL,
                                       .expression_node = no_ast_node,
                                       .is_document_focus = question.is_document_focus,
                                       .requires_grounded = true,
                                       .follows_consuming = question.follows_point},
                      motionless);
    if (status != SXML_STATUS_OK || !*motionless) {
        return status;
    }
    bool calls = false;
    status = subtree_calls_window_function(lowering, question.node, &calls);
    *motionless = !calls;
    return status;
}

/* Whether the attributes of a literal result element read nothing of the
   streamed node's content: then it only wraps its content. */
static sxml_status is_wrapping_element(const sxml_xslt_lowering *lowering, point_question question,
                                       bool *is_wrapping) {
    const sxml_xslt_ast_node *entry = node_of(lowering, question.node);
    *is_wrapping = entry->kind == SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT;
    for (uint32_t offset = 0; *is_wrapping && offset < entry->attributes.attribute_count;
         offset += 1) {
        const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
            lowering->ast, (sxml_xslt_ast_attribute_index){
                               .value = entry->attributes.first_attribute_index + offset});
        if (attribute->kind == SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS) {
            *is_wrapping = false;
            break;
        }
        const sxml_xslt_ast_expression *expression =
            expression_of_value(lowering, attribute->value);
        if (expression == NULL) {
            continue;
        }
        const sxml_status status =
            is_motionless(lowering,
                          (sweep_question){.instruction = {0},
                                           .assesses_content = false,
                                           .expression = expression,
                                           .expression_node = expression->ast->root,
                                           .is_document_focus = question.is_document_focus,
                                           .requires_grounded = true},
                          is_wrapping);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (*is_wrapping && calls_window_function(expression->ast)) {
            *is_wrapping = false;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Streamed paths */

/* Node kinds a streamed path's value goes through unchanged in focus:
   the path may be any of their operands, and each other operand must be
   motionless (XSLT 4.0, 19.8.1: they read the focus, a shallow window). */
static bool is_transparent_kind(sxml_ast_kind kind) {
    static const sxml_ast_kind kinds[] = {
        SXML_AST_KIND_SEQUENCE_EXPRESSION,
        SXML_AST_KIND_PARENTHESIZED_EXPRESSION,
        SXML_AST_KIND_IF_EXPRESSION,
        SXML_AST_KIND_OR_EXPRESSION,
        SXML_AST_KIND_AND_EXPRESSION,
        SXML_AST_KIND_COMPARISON_EXPRESSION,
        SXML_AST_KIND_OTHERWISE_EXPRESSION,
        SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION,
        SXML_AST_KIND_RANGE_EXPRESSION,
        SXML_AST_KIND_ADDITIVE_EXPRESSION,
        SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION,
        SXML_AST_KIND_UNION_EXPRESSION,
        SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION,
        SXML_AST_KIND_UNARY_EXPRESSION,
        SXML_AST_KIND_INSTANCE_OF_EXPRESSION,
        SXML_AST_KIND_TREAT_EXPRESSION,
        SXML_AST_KIND_CASTABLE_EXPRESSION,
        SXML_AST_KIND_CAST_EXPRESSION,
        SXML_AST_KIND_FUNCTION_CALL,
        SXML_AST_KIND_ARGUMENT_LIST,
        SXML_AST_KIND_KEYWORD_ARGUMENT,
    };
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index += 1) {
        if (kinds[index] == kind) {
            return true;
        }
    }
    return false;
}

/* Whether a child of a transparent node is an operand (not a name, a
   type or a cast target). */
static bool is_operand_field(sxml_ast_field field) {
    return field != SXML_AST_FIELD_NAME && field != SXML_AST_FIELD_TYPE;
}

/* Path kinds a streamed path is made of. */
static bool is_path_kind(sxml_ast_kind kind) {
    return kind == SXML_AST_KIND_RELATIVE_PATH_EXPRESSION ||
           kind == SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION || kind == SXML_AST_KIND_AXIS_STEP;
}

static sxml_ast_node_index unwrap_parentheses(const sxml_ast *ast, sxml_ast_node_index node) {
    while (sxml_ast_get_node(ast, node)->kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION) {
        const sxml_ast_node_index inner = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_EXPRESSION});
        if (inner.value == 0) {
            break;
        }
        node = inner;
    }
    return node;
}

/* The functions whose first operand a streamed path's nodes are atomized
   for (their parameter is atomic: XPath 4.0, 3.4.3, the coercion rules),
   and those that read only how many items it has. */
static const char *const atomizing_function_names[] = {"sum", "avg", "min", "max",
                                                       "distinct-values", "data", "string-join"};
static const char *const counting_function_names[] = {"count", "exists", "empty"};
static const char *const folding_function_names[] = {"sum", "min", "max"};
/* fn:avg of one argument folds as fn:sum and divides by the count (F&O
   4.0, 14.4.2: sum($values) div count($values); D228). */
static const char *const averaging_function_names[] = {"avg"};

/* Whether a function call is of the function namespace's name: no prefix
   (XSLT's default function namespace, 5.1.1) or Q{fn}. */
static bool is_standard_call(const sxml_ast *ast, sxml_ast_node_index call,
                             function_names names) {
    const sxml_ast_node_index name = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = call, .field = SXML_AST_FIELD_NAME});
    const sxml_ast_name *parts = &sxml_ast_get_node(ast, name)->payload.name;
    const sxml_byte_view prefix = sxml_ast_get_text(ast, parts->namespace_prefix);
    const sxml_byte_view uri = sxml_ast_get_text(ast, parts->namespace_uri);
    if (prefix.byte_count != 0 ||
        (uri.byte_count != 0 &&
         !sxml_byte_view_equals(uri, SXML_BYTE_VIEW_FROM_LITERAL(SXML_NAMESPACE_FUNCTIONS)))) {
        return false;
    }
    const sxml_byte_view local_name = called_name(ast, call);
    for (size_t name_index = 0; name_index < names.name_count; name_index += 1) {
        if (is_named(local_name, names.names[name_index])) {
            return true;
        }
    }
    return false;
}

/* The function call whose first argument path is (through
   parentheses); 0 for none. */
static sxml_ast_node_index counting_call_of(const sxml_ast *ast, sxml_ast_node_index path) {
    sxml_ast_node_index operand = path;
    sxml_ast_node_index parent = sxml_ast_get_node(ast, operand)->parent;
    while (parent.value != 0 &&
           sxml_ast_get_node(ast, parent)->kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION) {
        operand = parent;
        parent = sxml_ast_get_node(ast, operand)->parent;
    }
    if (parent.value == 0 || sxml_ast_get_node(ast, parent)->kind != SXML_AST_KIND_ARGUMENT_LIST ||
        sxml_ast_get_node(ast, parent)->first_child.value != operand.value ||
        sxml_ast_get_node(ast, operand)->next_sibling.value != 0) {
        return no_ast_node;
    }
    const sxml_ast_node_index call = sxml_ast_get_node(ast, parent)->parent;
    return sxml_ast_get_node(ast, call)->kind == SXML_AST_KIND_FUNCTION_CALL ? call : no_ast_node;
}

/* The grounding of a streamed path at path: by the function it is the
   first argument of. */
static stream_grounding grounding_of(const sxml_ast *ast, sxml_ast_node_index path,
                                     bool has_attribute_step) {
    sxml_ast_node_index operand = path;
    sxml_ast_node_index parent = sxml_ast_get_node(ast, operand)->parent;
    while (parent.value != 0 &&
           sxml_ast_get_node(ast, parent)->kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION) {
        operand = parent;
        parent = sxml_ast_get_node(ast, operand)->parent;
    }
    if (parent.value == 0 || sxml_ast_get_node(ast, parent)->kind != SXML_AST_KIND_ARGUMENT_LIST ||
        sxml_ast_get_node(ast, parent)->first_child.value != operand.value) {
        return SXML_STREAM_GROUNDING_NODES;
    }
    const sxml_ast_node_index call = sxml_ast_get_node(ast, parent)->parent;
    if (sxml_ast_get_node(ast, call)->kind != SXML_AST_KIND_FUNCTION_CALL) {
        return SXML_STREAM_GROUNDING_NODES;
    }
    if (is_standard_call(ast, call,
                         (function_names){.names = counting_function_names,
                                          .name_count = sizeof(counting_function_names) /
                                                        sizeof(counting_function_names[0])}) &&
        counting_call_of(ast, path).value == call.value) {
        /* The attributes of a path are not its last step's nodes: their
           values are counted (one each, no schema types in a streamed
           document). */
        return has_attribute_step ? SXML_STREAM_GROUNDING_ATOMIZED : SXML_STREAM_GROUNDING_COUNTED;
    }
    if (is_standard_call(ast, call,
                         (function_names){.names = folding_function_names,
                                          .name_count = sizeof(folding_function_names) /
                                                        sizeof(folding_function_names[0])}) &&
        counting_call_of(ast, path).value == call.value) {
        return SXML_STREAM_GROUNDING_FOLDED;
    }
    if (!has_attribute_step &&
        is_standard_call(ast, call,
                         (function_names){.names = averaging_function_names,
                                          .name_count = sizeof(averaging_function_names) /
                                                        sizeof(averaging_function_names[0])}) &&
        counting_call_of(ast, path).value == call.value) {
        /* One atomized value per element (no schema types in a streamed
           document): the counter counts the values. */
        return SXML_STREAM_GROUNDING_AVERAGED;
    }
    if (is_standard_call(ast, call,
                         (function_names){.names = atomizing_function_names,
                                          .name_count = sizeof(atomizing_function_names) /
                                                        sizeof(atomizing_function_names[0])})) {
        return SXML_STREAM_GROUNDING_ATOMIZED;
    }
    return SXML_STREAM_GROUNDING_NODES;
}

/* A streamed path found in an expression. */
typedef struct path_finding {
    const sxml_xslt_ast_expression *expression;
    bool is_document_focus;
    /* The path kind node is_streamed_path reads. */
    sxml_ast_node_index candidate;
    /* Out: the path's node (0 for none) and what it needs. */
    sxml_ast_node_index path;
    bool has_attribute_step;
    /* Out: false when the expression is not one streamed path with
       motionless operands around it. */
    bool is_found;
} path_finding;

/* Whether node, a path kind, is a streamed path: its steps (with a last
   attribute step that is motionless). */
static sxml_status is_streamed_path(const sxml_xslt_lowering *lowering, path_finding *finding,
                                    bool *is_path) {
    const sxml_xslt_ast_expression *expression = finding->expression;
    stream_path path;
    *is_path = read_path((path_reading){.ast = expression->ast,
                                        .node = finding->candidate,
                                        .is_document_focus = finding->is_document_focus,
                                        .allows_attribute = true},
                         &path);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 0; *is_path && status == SXML_STATUS_OK && index < path.step_count;
         index += 1U) {
        if (path.steps[index].predicate.value != 0) {
            status = is_motionless(lowering,
                                   (sweep_question){.instruction = {0},
                                                    .assesses_content = false,
                                                    .expression = expression,
                                                    .expression_node = path.steps[index].predicate,
                                                    .is_document_focus = false,
                                                    .requires_grounded = false},
                                   is_path);
        }
    }
    if (*is_path && status == SXML_STATUS_OK && path.attribute_step.value != 0) {
        status = is_motionless(lowering,
                               (sweep_question){.instruction = {0},
                                                .assesses_content = false,
                                                .expression = expression,
                                                .expression_node = path.attribute_step,
                                                .is_document_focus = false,
                                                .requires_grounded = false},
                               is_path);
    }
    finding->has_attribute_step = path.attribute_step.value != 0;
    return status;
}

/* Finds the one streamed path of finding->expression: the expression's
   nodes are walked down through transparent kinds (an explicit stack,
   D71); a path kind that is a streamed path is it, every other node met
   must be motionless. A simple map or filter whose left operand is the
   path runs its right operand on the path's grounded nodes. */
static sxml_status find_streamed_path(const sxml_xslt_lowering *lowering, path_finding *finding) {
    const sxml_ast *ast = finding->expression->ast;
    finding->path = (sxml_ast_node_index){0};
    finding->is_found = false;
    sxml_ast_node_index pending[64];
    size_t pending_count = 0;
    pending[pending_count] = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = ast->root, .field = SXML_AST_FIELD_BODY});
    pending_count += 1;
    bool is_valid = true;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && is_valid && pending_count > 0) {
        pending_count -= 1;
        const sxml_ast_node_index node = pending[pending_count];
        const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
        if (entry->kind == SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            is_valid = false;
            break;
        }
        sxml_ast_node_index candidate = node;
        if (entry->kind == SXML_AST_KIND_SIMPLE_MAP_EXPRESSION ||
            entry->kind == SXML_AST_KIND_FILTER_EXPRESSION) {
            candidate = unwrap_parentheses(ast, entry->first_child);
        }
        bool is_path = false;
        if (is_path_kind(sxml_ast_get_node(ast, candidate)->kind)) {
            finding->candidate = candidate;
            status = is_streamed_path(lowering, finding, &is_path);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (is_path) {
            is_valid = finding->path.value == 0;
            finding->path = candidate;
            continue;
        }
        if (candidate.value == node.value && is_transparent_kind(entry->kind)) {
            for (sxml_ast_node_index child = entry->first_child; child.value != 0;
                 child = sxml_ast_get_node(ast, child)->next_sibling) {
                if (!is_operand_field(sxml_ast_get_node(ast, child)->field)) {
                    continue;
                }
                if (pending_count == sizeof(pending) / sizeof(pending[0])) {
                    is_valid = false;
                    break;
                }
                pending[pending_count] = child;
                pending_count += 1;
            }
            continue;
        }
        status = is_motionless(lowering,
                               (sweep_question){.instruction = {0},
                                                .assesses_content = false,
                                                .expression = finding->expression,
                                                .expression_node = node,
                                                .is_document_focus = finding->is_document_focus,
                                                .requires_grounded = false},
                               &is_valid);
    }
    finding->is_found = status == SXML_STATUS_OK && is_valid && finding->path.value != 0;
    return status;
}

/* The accumulator functions (19.6, 19.7), which a streamed document
   answers for its windows when it computes its accumulators (D231). */
static const char *const accumulator_function_names[] = {"accumulator-before",
                                                         "accumulator-after"};

/* Whether the instruction, or a template rule it may apply, reads what a
   full window lacks: the size of the focus (fn:last), siblings
   (fn:path), or a function, template or attribute set sxml does not
   follow. For xsl:apply-templates, every template rule of its mode must
   be guaranteed-streamable (6.6.4). */
static sxml_status reads_beyond_windows(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index node, bool *reads_beyond) {
    sxml_status status = subtree_calls_window_function(lowering, node, reads_beyond);
    if (status != SXML_STATUS_OK || *reads_beyond ||
        node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_APPLY_TEMPLATES) {
        return status;
    }
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    bool is_guaranteed = false;
    status = sxml_xslt_assess_mode(&(sxml_xslt_mode_query){.ast = lowering->ast,
                                                           .allocator = lowerer->allocator,
                                                           .name_pool = lowerer->name_pool,
                                                           .instruction = node},
                                   &is_guaranteed);
    *reads_beyond = !is_guaranteed;
    for (uint32_t index = 1; status == SXML_STATUS_OK && !*reads_beyond &&
                             index < lowering->ast->node_count;
         index += 1U) {
        const sxml_xslt_ast_node_index template_rule = {.value = index};
        if (sxml_xslt_applies_in_mode(
                lowering->ast,
                (sxml_xslt_mode_pair){.instruction = node, .template_rule = template_rule})) {
            status = subtree_calls_window_function(lowering, template_rule, reads_beyond);
        }
    }
    return status;
}

/* Whether an attribute of xsl:for-each-group is evaluated with each
   selected item as the focus (14.1), on its full window. */
static bool is_item_attribute(sxml_xslt_ast_attribute_kind kind) {
    return kind == SXML_XSLT_AST_ATTRIBUTE_GROUP_BY ||
           kind == SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT ||
           kind == SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH ||
           kind == SXML_XSLT_AST_ATTRIBUTE_GROUP_ENDING_WITH;
}

/* Whether node is an instruction whose select expression has one
   streamed path (glossary `streamed_path`): xsl:value-of, xsl:copy-of,
   xsl:sequence, xsl:for-each (one that is not a stream point of its own),
   xsl:apply-templates, xsl:iterate and xsl:for-each-group. The instruction must be consuming and grounded
   (19.8.4), its other attributes motionless; what runs on the selected
   nodes (a body, sort keys, template rules) reads their full windows. */
typedef struct path_point_finding {
    stream_point point;
    bool is_point;
} path_point_finding;

static sxml_status is_path_point(const sxml_xslt_lowering *lowering, point_question question,
                                 path_point_finding *found) {
    bool *is_point = &found->is_point;
    *is_point = false;
    const sxml_xslt_ast_node_index node = question.node;
    const sxml_xslt_ast_node *entry = node_of(lowering, node);
    const bool runs_on_items = entry->kind == SXML_XSLT_AST_KIND_FOR_EACH ||
                               entry->kind == SXML_XSLT_AST_KIND_APPLY_TEMPLATES ||
                               entry->kind == SXML_XSLT_AST_KIND_ITERATE ||
                               entry->kind == SXML_XSLT_AST_KIND_FOR_EACH_GROUP;
    if ((!runs_on_items && entry->kind != SXML_XSLT_AST_KIND_VALUE_OF &&
         entry->kind != SXML_XSLT_AST_KIND_COPY_OF &&
         entry->kind != SXML_XSLT_AST_KIND_SEQUENCE) ||
        sxml_xslt_lowering_has_xslt_1_behaviour(lowering, node)) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *select = select_of(lowering, node);
    if (select == NULL || calls_window_function(select->ast)) {
        return SXML_STATUS_OK;
    }
    if (!runs_on_items) {
        /* The select is all: the content is xsl:fallback at most. */
        for (sxml_xslt_ast_node_index child = entry->first_child; child.value != 0;
             child = node_of(lowering, child)->next_sibling) {
            if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_FALLBACK) {
                return SXML_STATUS_OK;
            }
        }
    }
    sxml_xslt_stream_sweep sweep = SXML_XSLT_STREAM_SWEEP_UNCERTAIN;
    sxml_status status = sweep_of(lowering,
                                  (sweep_question){.instruction = node,
                                                   .assesses_content = false,
                                                   .expression = NULL,
                                                   .expression_node = no_ast_node,
                                                   .is_document_focus = question.is_document_focus,
                                                   .requires_grounded = true},
                                  &sweep);
    if (status != SXML_STATUS_OK || sweep != SXML_XSLT_STREAM_SWEEP_CONSUMING) {
        return status;
    }
    bool is_motionless_attribute = true;
    for (uint32_t offset = 0; is_motionless_attribute && offset < entry->attributes.attribute_count;
         offset += 1) {
        const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
            lowering->ast, (sxml_xslt_ast_attribute_index){
                               .value = entry->attributes.first_attribute_index + offset});
        const sxml_xslt_ast_expression *expression =
            expression_of_value(lowering, attribute->value);
        if (attribute->kind == SXML_XSLT_AST_ATTRIBUTE_SELECT || expression == NULL ||
            is_item_attribute(attribute->kind)) {
            continue;
        }
        status = is_motionless(lowering,
                               (sweep_question){.instruction = {0},
                                                .assesses_content = false,
                                                .expression = expression,
                                                .expression_node = expression->ast->root,
                                                .is_document_focus = question.is_document_focus,
                                                .requires_grounded = true},
                               &is_motionless_attribute);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (!is_motionless_attribute) {
        return SXML_STATUS_OK;
    }
    if (runs_on_items) {
        bool reads_beyond = true;
        status = reads_beyond_windows(lowering, node, &reads_beyond);
        if (status != SXML_STATUS_OK || reads_beyond) {
            return status;
        }
    }
    path_finding finding = {.expression = select,
                            .is_document_focus = question.is_document_focus,
                            .candidate = {0},
                            .path = {0},
                            .has_attribute_step = false,
                            .is_found = false};
    status = find_streamed_path(lowering, &finding);
    if (status != SXML_STATUS_OK || !finding.is_found) {
        return status;
    }
    const stream_grounding grounding =
        grounding_of(select->ast, finding.path, finding.has_attribute_step);
    found->point = (stream_point){
        .kind = SXML_STREAM_POINT_PATH,
        .node = node,
        .is_shallow = grounding == SXML_STREAM_GROUNDING_COUNTED || finding.has_attribute_step,
        .ast = select->ast,
        .path = finding.path,
        .grounding = grounding,
        .lowered = grounding == SXML_STREAM_GROUNDING_COUNTED ||
                           grounding == SXML_STREAM_GROUNDING_FOLDED ||
                           grounding == SXML_STREAM_GROUNDING_AVERAGED
                       ? counting_call_of(select->ast, finding.path)
                       : finding.path,
        .document = {0},
        .has_lowered = false};
    *is_point = true;
    return SXML_STATUS_OK;
}

/* Whether node is an xsl:apply-templates whose template rules can be
   applied to each node it selects as it passes (a stream point, D223):
   no xsl:sort or separator, its select a streamed path of steps (as
   xsl:for-each's), its parameters motionless, the instruction consuming
   and grounded, and its mode's template rules guaranteed-streamable and
   reading nothing a full window lacks. They see each node's position in
   the path; fn:last, which a streamed selection cannot answer, keeps them
   from streaming (19.8.9.14). */
/* The path of xsl:apply-templates without select at a node (6.6):
   child::node(), one step with no AST of its own. */
static stream_path default_select_path(void) {
    stream_path path;
    memset(&path, 0, sizeof(path));
    path.steps[0] = (stream_step){.step = {0}, .axis = SXML_AXIS_CHILD, .predicate = {0}};
    path.step_count = 1;
    return path;
}

typedef struct apply_point_finding {
    bool is_point;
    /* The path's last step is a child step: each node can get a shallow
       window, the template rule chosen for it streaming its content or
       grounding it (D228). */
    bool is_shallow;
} apply_point_finding;

static sxml_status is_apply_point(const sxml_xslt_lowering *lowering, point_question question,
                                  apply_point_finding *found) {
    bool *is_point = &found->is_point;
    *is_point = false;
    found->is_shallow = false;
    const sxml_xslt_ast_node_index node = question.node;
    if (node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_APPLY_TEMPLATES ||
        sxml_xslt_lowering_has_sort(lowering, node) ||
        has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SEPARATOR) ||
        sxml_xslt_lowering_has_xslt_1_behaviour(lowering, node)) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *select = select_of(lowering, node);
    stream_path path;
    if (!has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT)) {
        /* 6.6: no select is child::node() of a node (D228). */
        path = default_select_path();
    } else if (select == NULL || calls_window_function(select->ast) ||
               !read_path((path_reading){.ast = select->ast,
                                         .node = no_ast_node,
                                         .is_document_focus = question.is_document_focus,
                                         .allows_attribute = false},
                          &path)) {
        return SXML_STATUS_OK;
    }
    bool is_valid = true;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 0; is_valid && status == SXML_STATUS_OK && index < path.step_count;
         index += 1U) {
        if (path.steps[index].predicate.value != 0) {
            status = is_motionless(lowering,
                                   (sweep_question){.instruction = {0},
                                                    .assesses_content = false,
                                                    .expression = select,
                                                    .expression_node = path.steps[index].predicate,
                                                    .is_document_focus = false,
                                                    .requires_grounded = false},
                                   &is_valid);
        }
    }
    for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child;
         is_valid && status == SXML_STATUS_OK && child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        /* xsl:with-param is evaluated in the instruction's focus. */
        status = is_motionless(lowering,
                               (sweep_question){.instruction = child,
                                                .assesses_content = false,
                                                .expression = NULL,
                                                .expression_node = no_ast_node,
                                                .is_document_focus = question.is_document_focus,
                                                .requires_grounded = true},
                               &is_valid);
    }
    sxml_xslt_stream_sweep sweep = SXML_XSLT_STREAM_SWEEP_UNCERTAIN;
    if (is_valid && status == SXML_STATUS_OK) {
        status = sweep_of(lowering,
                          (sweep_question){.instruction = node,
                                           .assesses_content = false,
                                           .expression = NULL,
                                           .expression_node = no_ast_node,
                                           .is_document_focus = question.is_document_focus,
                                           .requires_grounded = true},
                          &sweep);
    }
    if (!is_valid || status != SXML_STATUS_OK || sweep != SXML_XSLT_STREAM_SWEEP_CONSUMING) {
        return status;
    }
    bool reads_beyond = true;
    status = reads_beyond_windows(lowering, node, &reads_beyond);
    *is_point = status == SXML_STATUS_OK && !reads_beyond;
    /* A descendant step's nodes inside a selected node are found in its
       full window. */
    found->is_shallow = path.steps[path.step_count - 1U].axis == SXML_AXIS_CHILD;
    return status;
}

/* Plans the bodies on the work list, the first (the xsl:source-document's)
   included: records the stream points and whether each body is streamed.
   *is_streamed is the first body's verdict. */
typedef struct body_list {
    stream_body *bodies;
    size_t count;
    size_t capacity;
} body_list;

static sxml_status push_body(const sxml_xslt_lowering *lowering, body_list *list,
                             stream_body body) {
    if (list->count == list->capacity) {
        const size_t capacity = list->capacity == 0 ? 8U : list->capacity * 2U;
        void *memory = NULL;
        const sxml_status status = sxml_xpath_lowerer_allocate_scratch(
            lowering->lowerer,
            (sxml_xpath_lowering_scratch_request){.element_count = capacity,
                                                  .element_size = sizeof(stream_body),
                                                  .element_alignment = alignof(stream_body)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (list->count > 0) {
            memcpy(memory, list->bodies, list->count * sizeof(stream_body));
        }
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, list->bodies);
        list->bodies = memory;
        list->capacity = capacity;
    }
    list->bodies[list->count] = body;
    list->count += 1;
    return SXML_STATUS_OK;
}

/* Plans the body of bodies[body_index]: its instructions (through
   wrapping elements, the explicit stack kept in nodes), adding the bodies
   of its stream points to list. */
typedef struct body_planning {
    const sxml_xslt_lowering *lowering;
    body_list *list;
    size_t body_index;
} body_planning;

/* The prongs of an xsl:fork in a body whose document can be read again
   (D231; XSLT 4.0, 16.1 and 18.3): when every child is an xsl:sequence,
   each becomes a PRONG point and a body of its own, a pass over the
   document; *has_prongs is false otherwise. */
static sxml_status add_prongs(const body_planning *planning, sxml_xslt_ast_node_index fork,
                              bool *has_prongs) {
    const sxml_xslt_lowering *lowering = planning->lowering;
    *has_prongs = node_of(lowering, fork)->first_child.value != 0;
    size_t prong_count = 0;
    for (sxml_xslt_ast_node_index child = node_of(lowering, fork)->first_child;
         *has_prongs && child.value != 0; child = node_of(lowering, child)->next_sibling) {
        const sxml_xslt_ast_kind kind = node_of(lowering, child)->kind;
        prong_count += kind == SXML_XSLT_AST_KIND_SEQUENCE ? 1U : 0U;
        *has_prongs = (kind == SXML_XSLT_AST_KIND_SEQUENCE || kind == SXML_XSLT_AST_KIND_FALLBACK) &&
                      prong_count <= STREAM_MAXIMUM_PRONG_COUNT;
    }
    sxml_status status = SXML_STATUS_OK;
    for (sxml_xslt_ast_node_index child = node_of(lowering, fork)->first_child;
         *has_prongs && status == SXML_STATUS_OK && child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_SEQUENCE) {
            continue;
        }
        status = add_point(lowering->stream_plan,
                           (stream_point){.kind = SXML_STREAM_POINT_PRONG,
                                          .node = child,
                                          .is_shallow = false,
                                          .ast = NULL,
                                          .path = no_ast_node,
                                          .grounding = SXML_STREAM_GROUNDING_NODES,
                                          .lowered = no_ast_node,
                                          .document = {0},
                                          .has_lowered = false});
        if (status == SXML_STATUS_OK) {
            status = push_body(lowering, planning->list,
                               (stream_body){.node = child,
                                             .point_number = lowering->stream_plan->point_count,
                                             .is_document_focus = true,
                                             .is_instruction = true,
                                             .allows_passes = true});
        }
    }
    return status;
}

static sxml_status plan_one_body(const body_planning *planning, bool *is_streamed) {
    const sxml_xslt_lowering *lowering = planning->lowering;
    const stream_body body = planning->list->bodies[planning->body_index];
    const bool is_document_focus = body.is_document_focus;
    *is_streamed = true;
    size_t point_count = 0;
    /* The siblings still to visit, one chain per open wrapping element. */
    sxml_xslt_ast_node_index pending[64];
    size_t pending_count = 0;
    pending[pending_count] =
        body.is_instruction ? body.node
                            : sxml_xslt_lowering_first_constructor_child(lowering->ast, body.node);
    pending_count += 1;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && *is_streamed && pending_count > 0) {
        const sxml_xslt_ast_node_index node = pending[pending_count - 1U];
        if (node.value == 0) {
            pending_count -= 1;
            continue;
        }
        /* A body that is one instruction has no siblings. */
        pending[pending_count - 1U] = body.is_instruction && pending_count == 1U
                                          ? (sxml_xslt_ast_node_index){.value = 0}
                                          : node_of(lowering, node)->next_sibling;
        if (body.allows_passes && is_document_focus &&
            node_of(lowering, node)->kind == SXML_XSLT_AST_KIND_FORK) {
            /* D231: each prong in a pass of its own, unless none reads
               the document's content. */
            bool motionless = false;
            status = is_motionless_instruction(
                lowering,
                (point_question){.node = node,
                                 .is_document_focus = true,
                                 .follows_point = point_count > 0},
                &motionless);
            if (status == SXML_STATUS_OK && !motionless) {
                bool has_prongs = false;
                status = add_prongs(planning, node, &has_prongs);
                *is_streamed = has_prongs;
            }
            continue;
        }
        if (node_of(lowering, node)->kind == SXML_XSLT_AST_KIND_SEQUENCE &&
            !has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT)) {
            /* Its content is the body's (10.4.1). */
            if (pending_count == sizeof(pending) / sizeof(pending[0])) {
                *is_streamed = false;
                break;
            }
            pending[pending_count] = node_of(lowering, node)->first_child;
            pending_count += 1;
            continue;
        }
        const point_question question = {
            .node = node, .is_document_focus = is_document_focus, .follows_point = point_count > 0};
        bool is_point = false;
        status = is_eligible_point(lowering, question, &is_point);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (is_point) {
            point_count += 1;
            status = add_point(lowering->stream_plan,
                               (stream_point){.kind = SXML_STREAM_POINT_FOR_EACH,
                                              .node = node,
                                              .is_shallow = false,
                                              .ast = NULL,
                                              .path = no_ast_node,
                                              .grounding = SXML_STREAM_GROUNDING_NODES,
                                              .lowered = no_ast_node,
                                              .document = {0},
                                              .has_lowered = false});
            if (status == SXML_STATUS_OK) {
                status =
                    push_body(lowering, planning->list,
                              (stream_body){.node = node,
                                            .point_number = lowering->stream_plan->point_count,
                                            .is_document_focus = false});
            }
            continue;
        }
        apply_point_finding apply_point = {.is_point = false, .is_shallow = false};
        status = is_apply_point(lowering, question, &apply_point);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (apply_point.is_point) {
            point_count += 1;
            status = add_point(lowering->stream_plan,
                               (stream_point){.kind = SXML_STREAM_POINT_APPLY,
                                              .node = node,
                                              .is_shallow = apply_point.is_shallow,
                                              .ast = NULL,
                                              .path = no_ast_node,
                                              .grounding = SXML_STREAM_GROUNDING_NODES,
                                              .lowered = no_ast_node,
                                              .document = {0},
                                              .has_lowered = false});
            continue;
        }
        path_point_finding path_point;
        status = is_path_point(lowering, question, &path_point);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (path_point.is_point) {
            point_count += 1;
            status = add_path_point(lowering, path_point.point);
            continue;
        }
        bool is_wrapping = false;
        status = is_wrapping_element(lowering, question, &is_wrapping);
        if (status == SXML_STATUS_OK && is_wrapping) {
            if (pending_count == sizeof(pending) / sizeof(pending[0])) {
                *is_streamed = false;
                break;
            }
            pending[pending_count] = node_of(lowering, node)->first_child;
            pending_count += 1;
            continue;
        }
        bool motionless = false;
        if (status == SXML_STATUS_OK) {
            status = is_motionless_instruction(lowering, question, &motionless);
        }
        *is_streamed = motionless;
    }
    *is_streamed = *is_streamed && point_count <= 1;
    return status;
}

/* Whether the stylesheet declares accumulators and an expression calls
   fn:accumulator-before or fn:accumulator-after: only then are the values
   of a streamed document's nodes read, and computed (D231). */
static bool calls_accumulator_functions(const sxml_xslt_ast *ast) {
    bool has_accumulators = false;
    for (uint32_t order = 1; !has_accumulators && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1U) {
        has_accumulators =
            sxml_xslt_ast_get_node(ast, sxml_xslt_ast_get_declaration(ast, order))->kind ==
            SXML_XSLT_AST_KIND_ACCUMULATOR;
    }
    if (!has_accumulators) {
        return false;
    }
    const function_names names = {.names = accumulator_function_names,
                                  .name_count = sizeof(accumulator_function_names) /
                                                sizeof(accumulator_function_names[0])};
    for (uint32_t index = 1; index < ast->expression_count; index += 1U) {
        const sxml_xslt_ast_expression *expression = sxml_xslt_ast_get_expression(
            ast, (sxml_xslt_ast_expression_index){.value = index});
        if (expression->ast != NULL && calls_named_function(expression->ast, names)) {
            return true;
        }
    }
    return false;
}

/* Plans the streamed body of node (an xsl:source-document or a template
   rule) and the bodies of its stream points; *is_streamed is the verdict
   of node's own body. Points of a body not streamed are dropped. */
static sxml_status plan_streamed_body(const sxml_xslt_lowering *lowering, stream_body first,
                                      bool *is_streamed) {
    *is_streamed = false;
    sxml_xslt_stream_plan *plan = lowering->stream_plan;
    const size_t first_point = plan->point_count;
    body_list list = {.bodies = NULL, .count = 0, .capacity = 0};
    sxml_status status = push_body(lowering, &list, first);
    for (size_t body_index = 0; status == SXML_STATUS_OK && body_index < list.count;
         body_index += 1) {
        bool is_body_streamed = false;
        status = plan_one_body(
            &(body_planning){.lowering = lowering, .list = &list, .body_index = body_index},
            &is_body_streamed);
        const size_t point_number = list.bodies[body_index].point_number;
        if (point_number == 0 ||
            (!is_body_streamed &&
             plan->points[point_number - 1U].kind == SXML_STREAM_POINT_PRONG)) {
            /* A prong that does not stream leaves the fork to the whole
               document (D231). */
            *is_streamed = is_body_streamed;
            if (!is_body_streamed) {
                break;
            }
        } else {
            plan->points[point_number - 1U].is_shallow = is_body_streamed;
        }
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, list.bodies);
    for (size_t point_index = first_point; point_index < plan->point_count; point_index += 1) {
        plan->points[point_index].document = first.node;
    }
    if (status != SXML_STATUS_OK || !*is_streamed) {
        /* Not streamed: its points are lowered as before. */
        drop_points(lowering, first_point);
        *is_streamed = false;
    }
    return status;
}

sxml_status sxml_xslt_plan_source_document(const sxml_xslt_lowering *lowering,
                                           sxml_xslt_ast_node_index node, bool *is_streamed) {
    *is_streamed = false;
    sxml_xslt_stream_plan *plan = lowering->stream_plan;
    const sxml_xslt_ast_attribute *streamable =
        attribute_of(lowering, node, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE);
    if (plan == NULL || streamable->value.kind != SXML_XSLT_AST_VALUE_KIND_BOOLEAN ||
        !streamable->value.payload.is_true ||
        has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_VALIDATION) ||
        has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_TYPE) ||
        sxml_xslt_lowering_has_xslt_1_behaviour(lowering, node)) {
        return SXML_STATUS_OK;
    }
    bool reads_accumulators = false;
    const sxml_status status = sxml_xslt_reads_accumulators(
        &(sxml_xslt_accumulator_reading){.ast = lowering->ast,
                                         .allocator = lowering->lowerer->allocator,
                                         .name_pool = lowering->lowerer->name_pool},
        &reads_accumulators);
    if (status != SXML_STATUS_OK || reads_accumulators) {
        return status;
    }
    return plan_streamed_body(
        lowering, (stream_body){.node = node,
                                .point_number = 0,
                                .is_document_focus = true,
                                .is_instruction = false,
                                .allows_passes = true},
        is_streamed);
}

sxml_status sxml_xslt_reads_accumulators(const sxml_xslt_accumulator_reading *reading,
                                         bool *reads) {
    *reads = false;
    if (!calls_accumulator_functions(reading->ast)) {
        return SXML_STATUS_OK;
    }
    *reads = true;
    bool is_guaranteed = false;
    const sxml_status status = sxml_xslt_assess_accumulators(
        &(sxml_xslt_accumulator_query){.ast = reading->ast,
                                       .allocator = reading->allocator,
                                       .name_pool = reading->name_pool},
        &is_guaranteed);
    *reads = !is_guaranteed;
    return status;
}

sxml_status sxml_xslt_stream_accumulators(const sxml_xslt_streamed_accumulator_request *request,
                                          sxml_ir_stream_accumulators *functions) {
    *functions = (sxml_ir_stream_accumulators){
        .initial = {0}, .start = {0}, .end = {0}, .descend = {0}, .capture = {0}};
    if (!calls_accumulator_functions(request->lowering->ast)) {
        /* Nothing reads the values: they need not be computed. */
        return SXML_STATUS_OK;
    }
    return sxml_xslt_prepare_streamed_accumulators(request, functions);
}

/* Whether the parameters of a template rule read nothing of its node's
   content: none has a default that is not motionless. */
static sxml_status has_motionless_parameters(const sxml_xslt_lowering *lowering,
                                             sxml_xslt_template_rule_plan rule,
                                             bool *motionless) {
    *motionless = true;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_xslt_ast_node_index child = node_of(lowering, rule.node)->first_child;
         *motionless && status == SXML_STATUS_OK && child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_PARAM &&
            (has_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_SELECT) ||
             node_of(lowering, child)->first_child.value != 0)) {
            status = is_motionless_instruction(
                lowering,
                (point_question){.node = child, .is_document_focus = rule.is_document_focus},
                motionless);
        }
    }
    return status;
}

sxml_status sxml_xslt_plan_template_rule(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_template_rule_plan rule, bool *is_streamed) {
    *is_streamed = false;
    if (lowering->stream_plan == NULL || sxml_xslt_lowering_has_xslt_1_behaviour(lowering, rule.node)) {
        return SXML_STATUS_OK;
    }
    bool motionless = false;
    sxml_status status = has_motionless_parameters(lowering, rule, &motionless);
    if (status != SXML_STATUS_OK || !motionless) {
        return status;
    }
    return plan_streamed_body(lowering,
                              (stream_body){.node = rule.node,
                                            .point_number = 0,
                                            .is_document_focus = rule.is_document_focus},
                              is_streamed);
}

sxml_status sxml_xslt_check_streamed_body(const sxml_xslt_lowering *lowering,
                                          sxml_xslt_ast_node_index node) {
    const sxml_xslt_stream_plan *plan = lowering->stream_plan;
    for (size_t point_index = 0; plan != NULL && point_index < plan->point_count;
         point_index += 1) {
        const stream_point *point = &plan->points[point_index];
        if (point->kind == SXML_STREAM_POINT_PATH && point->document.value == node.value &&
            !point->has_lowered) {
            /* A streamed path lowered another way would read the body's
               shallow window as a tree. */
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------- Lowering a chain of STREAMs */

/* The slots of one step's focus, and its node test. */
typedef struct step_lowering {
    sxml_ir_slot value;
    sxml_ir_slot position;
    sxml_type_identifier node_test;
    sxml_xpath_lowering_focus predicate_focus;
    sxml_ir_expression_index predicate;
} step_lowering;

/* A path lowered to a chain of STREAMs, one per step (a stream point's
   select, or a streamed path), with the body of the last in the focus of
   the node it selects. */
typedef struct stream_chain {
    /* The path's AST and static context (NULL: the lowerer's current). */
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    stream_path path;
    step_lowering steps[STREAM_MAXIMUM_STEP_COUNT];
    uint32_t next_predicate;
    sxml_ir_atom origin;
    /* Counts the nodes the last step selects: the path's position (more
       than one step), or how many there are (a counted path). */
    bool needs_counter;
    sxml_ir_slot counter;
    /* An aggregate folded in a slot, bound to () first (a folded path). */
    bool needs_fold;
    sxml_ir_slot fold;
    bool is_shallow;
    sxml_xpath_lowering_focus item_focus;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
} stream_chain;

/* Resolves the chain's node tests, makes its slots and reads its origin
   (the focus) in block. */
typedef struct chain_start {
    sxml_xpath_lowerer *lowerer;
    sxml_xpath_lowering_block *block;
} chain_start;

static sxml_status start_chain(chain_start start, stream_chain *chain) {
    sxml_xpath_lowerer *lowerer = start.lowerer;
    const sxml_xpath_static_context *static_context =
        chain->static_context != NULL ? chain->static_context : lowerer->static_context;
    sxml_status status = sxml_xpath_lowerer_read_focus(
        lowerer,
        (sxml_xpath_lowering_focus_read){.block = start.block,
                                         .node = no_ast_node,
                                         .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
        &chain->origin);
    for (uint32_t index = 0; status == SXML_STATUS_OK && index < chain->path.step_count;
         index += 1U) {
        step_lowering *step = &chain->steps[index];
        const stream_step *read = &chain->path.steps[index];
        if (read->step.value == 0) {
            /* The default select's node(). */
            step->node_test = sxml_builtin_type_identifier(SXML_TYPE_NODE);
        } else {
            status = sxml_xpath_lowerer_resolve_step_test(
            lowerer,
            &(sxml_xpath_step_test_request){
                .ast = chain->ast,
                .static_context = static_context,
                .test = sxml_ast_find_child(
                    chain->ast,
                    (sxml_ast_child_query){.parent = read->step, .field = SXML_AST_FIELD_TEST}),
                .axis = read->axis},
            &step->node_test);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_slot(lowerer, &step->value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_slot(lowerer, &step->position);
        }
        step->predicate_focus =
            (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
                                        .slots = {.context_value = step->value,
                                                  .context_position = step->position,
                                                  .context_size = {0}}};
    }
    if (status == SXML_STATUS_OK && (chain->path.step_count > 1U || chain->needs_counter)) {
        /* The path's position: a slot the chain's last STREAM counts in,
           bound to 0 before it. */
        sxml_ir_atom zero = {0};
        status =
            sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_from_integer(0), &zero);
        sxml_ir_atom_list list = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_atoms(
                lowerer->unit, (sxml_ir_atom_view){.atoms = &zero, .atom_count = 1}, &list);
        }
        sxml_ir_atom counter = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_bind_expression(
                lowerer,
                &(sxml_xslt_expression_to_bind){
                    .block = start.block,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                   .sequence = list}},
                &counter);
        }
        if (status == SXML_STATUS_OK && counter.atom_kind != SXML_IR_ATOM_KIND_SLOT) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        chain->counter = counter.slot;
    }
    if (status == SXML_STATUS_OK && chain->needs_fold) {
        sxml_ir_expression_index empty = {0};
        status = sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &empty);
        sxml_ir_atom fold = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_bind_expression(
                lowerer,
                (sxml_xpath_lowering_expression_in_block){.block = start.block,
                                                          .expression = empty},
                &fold);
        }
        if (status == SXML_STATUS_OK && fold.atom_kind != SXML_IR_ATOM_KIND_SLOT) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        chain->fold = fold.slot;
    }
    const step_lowering *last = &chain->steps[chain->path.step_count - 1U];
    chain->item_focus =
        (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
                                    .slots = {.context_value = last->value,
                                              .context_position = last->position,
                                              .context_size = {0}}};
    chain->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    return status;
}

/* Lowers the chain's next predicate as a body in its step's focus. */
static sxml_status call_next_predicate(sxml_xpath_lowering_machine *machine, stream_chain *chain,
                                       bool *has_called) {
    *has_called = false;
    while (chain->next_predicate < chain->path.step_count) {
        const uint32_t index = chain->next_predicate;
        chain->next_predicate += 1U;
        if (chain->path.steps[index].predicate.value == 0) {
            continue;
        }
        *has_called = true;
        return sxml_xpath_lowering_call_embedded(
            machine, &(sxml_xpath_lowering_embedded_call){
                         .embedding = {.ast = chain->ast,
                                       .node = chain->path.steps[index].predicate,
                                       .static_context = chain->static_context,
                                       .focus = &chain->steps[index].predicate_focus},
                         .block = NULL,
                         .is_body = true,
                         .destination = {.expression = &chain->steps[index].predicate,
                                         .atom = NULL,
                                         .atom_block = NULL,
                                         .focused = NULL}});
    }
    return SXML_STATUS_OK;
}

/* The chain of STREAMs, made the last first, its body chain->body in
   chain->body_block. */
static sxml_status make_chain(sxml_xpath_lowerer *lowerer, const stream_chain *chain,
                              sxml_ir_expression_index *expression) {
    for (uint32_t index = 0; index < chain->path.step_count; index += 1U) {
        if (chain->steps[index].predicate_focus.parts_read.context_size.value != 0) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if (chain->item_focus.parts_read.context_size.value != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_expression_index body = {0};
    sxml_ir_expression_index result = {0};
    sxml_status status = sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = chain->body},
        &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer,
            (sxml_xpath_lowering_block_result){.block = chain->body_block, .result = result},
            &body);
    }
    for (uint32_t index = chain->path.step_count; status == SXML_STATUS_OK && index > 0;
         index -= 1U) {
        const step_lowering *step = &chain->steps[index - 1U];
        const bool is_last = index == chain->path.step_count;
        const sxml_ir_atom origin =
            index == 1U ? chain->origin : sxml_ir_slot_atom(chain->steps[index - 2U].value);
        const uint32_t flags =
            !is_last || chain->is_shallow ? (uint32_t)SXML_IR_STREAM_SHALLOW_WINDOWS : 0U;
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_STREAM,
                .stream = {.origin = origin,
                           .axis = chain->path.steps[index - 1U].axis,
                           .node_test = step->node_test,
                           .predicate = step->predicate,
                           .focus = {.context_value = step->value,
                                     .context_position = step->position,
                                     .context_size = {0}},
                           .position_counter = is_last ? chain->counter : (sxml_ir_slot){0},
                           .body = body,
                           .flags = flags,
                           .fold = is_last ? chain->fold : (sxml_ir_slot){0}}},
            &body);
    }
    *expression = body;
    return status;
}

/* Finishes frame with the chain of STREAMs. */
static sxml_status finish_chain(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame, const stream_chain *chain) {
    sxml_ir_expression_index expression = {0};
    const sxml_status status = make_chain(machine->state, chain, &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, expression);
}

enum {
    SXML_STREAM_CHAIN_STEP_START = 0,
    SXML_STREAM_CHAIN_STEP_PREDICATES = 1,
    SXML_STREAM_CHAIN_STEP_BODY = 2,
    /* A streamed path: its body's grounding. */
    SXML_STREAM_CHAIN_STEP_GROUNDING = 3,
    SXML_STREAM_CHAIN_STEP_FINISH = 4,
};

/* --------------------------------------------- Lowering a stream point */

typedef struct stream_point_locals {
    sxml_xslt_instruction_locals head;
    sxml_xpath_lowering_block *block;
    bool applies_templates;
    stream_chain chain;
} stream_point_locals;

static sxml_status continue_stream_point(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    stream_point_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    stream_chain *chain = &locals->chain;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == SXML_STREAM_CHAIN_STEP_START) {
        frame->step = SXML_STREAM_CHAIN_STEP_PREDICATES;
        const stream_point *point = find_point(lowering, locals->head.node);
        const sxml_xslt_ast_expression *select = select_of(lowering, locals->head.node);
        if (point != NULL && point->kind == SXML_STREAM_POINT_APPLY &&
            !has_attribute(lowering, locals->head.node, SXML_XSLT_AST_ATTRIBUTE_SELECT)) {
            chain->path = default_select_path();
            chain->ast = NULL;
            chain->static_context = NULL;
        } else if (point == NULL || select == NULL ||
                   !read_path((path_reading){.ast = select->ast,
                                             .node = no_ast_node,
                                             .is_document_focus = true,
                                             .allows_attribute = false},
                              &chain->path)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        } else {
            chain->ast = select->ast;
            chain->static_context = &lowering->static_contexts[select->static_context.value];
        }
        chain->is_shallow = point->is_shallow;
        locals->applies_templates = point->kind == SXML_STREAM_POINT_APPLY;
        status = start_chain(
            (chain_start){.lowerer = machine->state, .block = frame->block}, chain);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (frame->step == SXML_STREAM_CHAIN_STEP_PREDICATES) {
        bool has_called = false;
        status = call_next_predicate(machine, chain, &has_called);
        if (status != SXML_STATUS_OK || has_called) {
            return status;
        }
        frame->step = SXML_STREAM_CHAIN_STEP_BODY;
    }
    if (frame->step == SXML_STREAM_CHAIN_STEP_BODY) {
        frame->step = SXML_STREAM_CHAIN_STEP_FINISH;
        if (locals->applies_templates) {
            return sxml_xslt_lowering_call_focused_apply_templates(
                machine, &(sxml_xslt_focused_apply_call){.lowering = lowering,
                                                         .node = locals->head.node,
                                                         .block = &chain->body_block,
                                                         .item_focus = chain->item_focus.slots,
                                                         .atom = &chain->body});
        }
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = sxml_xslt_lowering_first_constructor_child(lowering->ast,
                                                                             locals->head.node),
                         .block = &chain->body_block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &chain->body,
                                         .atom_block = &chain->body_block,
                                         .focused = NULL},
                         .focus = &chain->item_focus});
    }
    return finish_chain(machine, frame, chain);
}

static const sxml_xpath_lowering_procedure stream_point_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("stream point"),
    .step = continue_stream_point,
    .locals_size = sizeof(stream_point_locals)};

sxml_status sxml_xslt_lowering_call_stream_point(sxml_xpath_lowering_machine *machine,
                                                 const sxml_xslt_stream_call *call) {
    const stream_point_locals initial = {.head = {.lowering = call->lowering, .node = call->node},
                                         .block = call->block};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &stream_point_procedure,
            .node = no_ast_node,
            .block = call->block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = call->atom,
                            .atom_block = call->block,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
}

/* ------------------------------------------- Lowering a streamed path */

/* The frame of a lowering override (src/xpath_lower_internal.h): its
   state is the lowering, its number the PATH point's index. */
typedef struct streamed_path_locals {
    sxml_xpath_lowering_override_locals head;
    stream_point point;
    stream_chain chain;
    /* The attribute step's value, or the selected node. */
    sxml_ir_atom item;
} streamed_path_locals;

/* The body of a streamed path: the grounding of what it selects
   (stream_grounding). */
static sxml_status ground_item(sxml_xpath_lowerer *lowerer, streamed_path_locals *locals) {
    stream_chain *chain = &locals->chain;
    switch (locals->point.grounding) {
    case SXML_STREAM_GROUNDING_NODES:
        chain->body = locals->item;
        return SXML_STATUS_OK;
    case SXML_STREAM_GROUNDING_ATOMIZED:
        /* XPath 4.0, 2.5.3: what the function's coercion would do. */
        return sxml_xslt_lowering_bind_library_call(
            lowerer,
            &(sxml_xslt_library_call){.block = &chain->body_block,
                                      .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                      .local_name = SXML_BYTE_VIEW_FROM_LITERAL("data"),
                                      .arguments = {.atoms = &locals->item, .atom_count = 1}},
            &chain->body);
    case SXML_STREAM_GROUNDING_FOLDED:
    case SXML_STREAM_GROUNDING_AVERAGED: {
        /* fn:F((the aggregate so far, fn:data(node))); fn:sum for avg. */
        sxml_ir_atom value = {0};
        sxml_status status = sxml_xslt_lowering_bind_library_call(
            lowerer,
            &(sxml_xslt_library_call){.block = &chain->body_block,
                                      .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                      .local_name = SXML_BYTE_VIEW_FROM_LITERAL("data"),
                                      .arguments = {.atoms = &locals->item, .atom_count = 1}},
            &value);
        const sxml_ir_atom pair[2] = {sxml_ir_slot_atom(chain->fold), value};
        sxml_ir_atom_list list = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_atoms(
                lowerer->unit, (sxml_ir_atom_view){.atoms = pair, .atom_count = 2}, &list);
        }
        sxml_ir_atom sequence = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_bind_expression(
                lowerer,
                &(sxml_xslt_expression_to_bind){
                    .block = &chain->body_block,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                   .sequence = list}},
                &sequence);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xslt_lowering_bind_library_call(
            lowerer,
            &(sxml_xslt_library_call){.block = &chain->body_block,
                                      .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                      .local_name =
                                          locals->point.grounding == SXML_STREAM_GROUNDING_AVERAGED
                                              ? SXML_BYTE_VIEW_FROM_LITERAL("sum")
                                              : called_name(lowerer->ast, locals->point.lowered),
                                      .arguments = {.atoms = &sequence, .atom_count = 1}},
            &chain->body);
    }
    case SXML_STREAM_GROUNDING_COUNTED:
    {
        /* The chain's counter counts the nodes; the body gives none. */
        sxml_ir_expression_index empty = {0};
        const sxml_status status =
            sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &empty);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = &chain->body_block,
                                                      .expression = empty},
            &chain->body);
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* A counted or folded path's value, once the chain (bound first) has
   run: fn:count, fn:exists or fn:empty of its nodes from the chain's
   counter, or fn:sum, fn:min or fn:max of the fold (which is () for no
   node). */
static sxml_status finish_aggregate_path(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame,
                                       const streamed_path_locals *locals) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const stream_chain *chain = &locals->chain;
    sxml_ir_expression_index expression = {0};
    sxml_status status = make_chain(lowerer, chain, &expression);
    sxml_ir_atom ignored = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = frame->block,
                                                      .expression = expression},
            &ignored);
    }
    const bool is_averaged = locals->point.grounding == SXML_STREAM_GROUNDING_AVERAGED;
    const bool is_folded = locals->point.grounding == SXML_STREAM_GROUNDING_FOLDED;
    sxml_ir_atom count = sxml_ir_slot_atom(is_folded ? chain->fold : chain->counter);
    const sxml_byte_view local_name = called_name(lowerer->ast, locals->point.lowered);
    sxml_ir_atom result = count;
    if (status == SXML_STATUS_OK && is_averaged) {
        /* The sum, () for no node, div the count (F&O 4.0, 14.4.2). */
        status = sxml_xslt_lowering_bind_expression(
            lowerer, &(sxml_xslt_expression_to_bind){
                         .block = frame->block,
                         .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                                        .binary = {.operator = SXML_IR_BINARY_OPERATOR_DIVIDE,
                                                   .left_operand = sxml_ir_slot_atom(chain->fold),
                                                   .right_operand = count}}},
            &result);
    } else if (status == SXML_STATUS_OK && is_folded) {
        status = sxml_xslt_lowering_bind_library_call(
            lowerer,
            &(sxml_xslt_library_call){.block = frame->block,
                                      .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                      .local_name = local_name,
                                      .arguments = {.atoms = &count, .atom_count = 1}},
            &result);
    } else if (status == SXML_STATUS_OK && !is_named(local_name, "count")) {
        /* XPath 4.0, F&O 4.0, 14.2.3 and 14.2.4: exists is a count above
           zero, empty a count of zero. */
        status = sxml_xslt_lowering_bind_library_call(
            lowerer,
            &(sxml_xslt_library_call){.block = frame->block,
                                      .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                      .local_name = is_named(local_name, "exists")
                                                        ? SXML_BYTE_VIEW_FROM_LITERAL("boolean")
                                                        : SXML_BYTE_VIEW_FROM_LITERAL("not"),
                                      .arguments = {.atoms = &count, .atom_count = 1}},
            &result);
    }
    sxml_ir_expression_index value = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = result},
            &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, value);
}

static sxml_status continue_streamed_path(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    streamed_path_locals *locals = frame->locals;
    stream_chain *chain = &locals->chain;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == SXML_STREAM_CHAIN_STEP_START) {
        frame->step = SXML_STREAM_CHAIN_STEP_PREDICATES;
        const sxml_xslt_lowering *lowering = locals->head.state;
        sxml_xslt_stream_plan *plan = lowering->stream_plan;
        if (locals->head.number >= plan->point_count) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        locals->point = plan->points[locals->head.number];
        /* The same path planned again (its template lowered again) is
           lowered by the first override. */
        for (size_t point_index = 0; point_index < plan->point_count; point_index += 1) {
            stream_point *same = &plan->points[point_index];
            if (same->kind == SXML_STREAM_POINT_PATH && same->ast == locals->point.ast &&
                same->path.value == locals->point.path.value) {
                same->has_lowered = true;
            }
        }
        if (!read_path((path_reading){.ast = lowerer->ast,
                                      .node = locals->point.path,
                                      .is_document_focus = true,
                                      .allows_attribute = true},
                       &chain->path)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        chain->ast = lowerer->ast;
        chain->static_context = NULL;
        chain->is_shallow = locals->point.is_shallow;
        chain->needs_counter = locals->point.grounding == SXML_STREAM_GROUNDING_COUNTED ||
                               locals->point.grounding == SXML_STREAM_GROUNDING_AVERAGED;
        chain->needs_fold = locals->point.grounding == SXML_STREAM_GROUNDING_FOLDED ||
                            locals->point.grounding == SXML_STREAM_GROUNDING_AVERAGED;
        status = start_chain((chain_start){.lowerer = lowerer, .block = frame->block}, chain);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (frame->step == SXML_STREAM_CHAIN_STEP_PREDICATES) {
        bool has_called = false;
        status = call_next_predicate(machine, chain, &has_called);
        if (status != SXML_STATUS_OK || has_called) {
            return status;
        }
        frame->step = SXML_STREAM_CHAIN_STEP_BODY;
    }
    if (frame->step == SXML_STREAM_CHAIN_STEP_BODY) {
        frame->step = SXML_STREAM_CHAIN_STEP_GROUNDING;
        locals->item = sxml_ir_slot_atom(chain->item_focus.slots.context_value);
        if (chain->path.attribute_step.value != 0) {
            /* E/@a: the attribute step in the focus of each element. */
            return sxml_xpath_lowering_call_embedded(
                machine, &(sxml_xpath_lowering_embedded_call){
                             .embedding = {.ast = chain->ast,
                                           .node = chain->path.attribute_step,
                                           .static_context = NULL,
                                           .focus = &chain->item_focus},
                             .block = &chain->body_block,
                             .is_body = false,
                             .destination = {.expression = NULL,
                                             .atom = &locals->item,
                                             .atom_block = &chain->body_block,
                                             .focused = NULL}});
        }
    }
    if (frame->step == SXML_STREAM_CHAIN_STEP_GROUNDING) {
        frame->step = SXML_STREAM_CHAIN_STEP_FINISH;
        status = ground_item(lowerer, locals);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (locals->point.grounding == SXML_STREAM_GROUNDING_COUNTED ||
        locals->point.grounding == SXML_STREAM_GROUNDING_FOLDED ||
        locals->point.grounding == SXML_STREAM_GROUNDING_AVERAGED) {
        return finish_aggregate_path(machine, frame, locals);
    }
    return finish_chain(machine, frame, chain);
}

static const sxml_xpath_lowering_procedure streamed_path_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("streamed path"),
    .step = continue_streamed_path,
    .locals_size = sizeof(streamed_path_locals)};

/* ------------------------------------- Lowering a streamed document body */

typedef struct streamed_document_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom href;
    sxml_xpath_lowering_focus document_focus;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
    /* A prong of an xsl:fork (head.node its xsl:sequence, D231), a pass
       of its own over the document of scope, the xsl:source-document. */
    bool is_prong;
    sxml_xslt_ast_node_index scope;
    /* The plan's document_href before this document's. */
    sxml_ir_atom saved_href;
} streamed_document_locals;

static sxml_status continue_streamed_document(sxml_xpath_lowering_machine *machine,
                                              sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    streamed_document_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        sxml_ir_slot value = {0};
        sxml_ir_slot position = {0};
        sxml_status status = sxml_xpath_lowerer_add_slot(lowerer, &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_slot(lowerer, &position);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->document_focus = (sxml_xpath_lowering_focus){
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
            .slots = {.context_value = value, .context_position = position, .context_size = {0}}};
        locals->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
        sxml_xslt_stream_plan *plan = lowering->stream_plan;
        locals->saved_href = plan->document_href;
        if (!locals->is_prong) {
            locals->scope = locals->head.node;
            plan->document_href = locals->href;
        }
        const sxml_xslt_ast_attribute *select =
            attribute_of(lowering, locals->head.node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        if (locals->is_prong && select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
            /* xsl:sequence select (10.4.1). */
            return sxml_xslt_lowering_call_expression(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression = select->value.payload.expression,
                                                      .block = &locals->body_block,
                                                      .atom = &locals->body,
                                                      .focus = &locals->document_focus});
        }
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = sxml_xslt_lowering_first_constructor_child(lowering->ast,
                                                                             locals->head.node),
                         .block = &locals->body_block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &locals->body,
                                         .atom_block = &locals->body_block,
                                         .focused = NULL},
                         .focus = &locals->document_focus});
    }
    lowering->stream_plan->document_href = locals->saved_href;
    if (locals->document_focus.parts_read.context_size.value != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_xslt_check_streamed_body(lowering, locals->head.node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_expression_index result = {0};
    sxml_ir_expression_index body = {0};
    status = sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = locals->body},
        &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer,
            (sxml_xpath_lowering_block_result){.block = locals->body_block, .result = result},
            &body);
    }
    /* 19.2: the accumulators use-accumulators names, computed as the
       document's events pass (D231). */
    sxml_ir_stream_accumulators accumulators = {
        .initial = {0}, .start = {0}, .end = {0}, .descend = {0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_stream_accumulators(
            &(sxml_xslt_streamed_accumulator_request){
                .lowering = lowering, .scope = locals->scope, .invocation = NULL},
            &accumulators);
    }
    sxml_ir_expression_index stream = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_STREAM,
                                 .stream = {.origin = locals->href,
                                            .axis = SXML_AXIS_SELF,
                                            .node_test = {.index = SXML_TYPE_NONE},
                                            .predicate = {0},
                                            .focus = locals->document_focus.slots,
                                            .position_counter = {0},
                                            .body = body,
                                            .flags = (uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT,
                                            .fold = {0},
                                            .accumulators = accumulators}},
            &stream);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, stream);
}

static const sxml_xpath_lowering_procedure streamed_document_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("streamed xsl:source-document"),
    .step = continue_streamed_document,
    .locals_size = sizeof(streamed_document_locals)};

sxml_status sxml_xslt_lowering_call_streamed_document(sxml_xpath_lowering_machine *machine,
                                                      const sxml_xslt_stream_call *call) {
    const streamed_document_locals initial = {
        .head = {.lowering = call->lowering, .node = call->node}, .href = call->href};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &streamed_document_procedure,
            .node = no_ast_node,
            .block = call->block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = call->atom,
                            .atom_block = call->block,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
}

/* ---------------------------------------------------------- xsl:fork */

bool sxml_xslt_is_streamed_fork(const sxml_xslt_lowering *lowering,
                                sxml_xslt_ast_node_index node) {
    for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child; child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_SEQUENCE) {
            const stream_point *point = find_point(lowering, child);
            return point != NULL && point->kind == SXML_STREAM_POINT_PRONG;
        }
    }
    return false;
}

/* A streamed xsl:fork (D231; XSLT 4.0, 16.1 and 18.3): each prong a
   STREAM with OPENS_DOCUMENT over the href of the document again, in a
   pass of its own, the values concatenated in the order of the prongs. */
typedef struct streamed_fork_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom href;
    /* The prong lowered last, and the values. */
    sxml_xslt_ast_node_index prong;
    sxml_ir_atom values[STREAM_MAXIMUM_PRONG_COUNT];
    size_t value_count;
} streamed_fork_locals;

/* The xsl:sequence child after prong (the first for 0), or 0. */
static sxml_xslt_ast_node_index next_prong(const sxml_xslt_lowering *lowering,
                                           streamed_fork_locals *locals) {
    sxml_xslt_ast_node_index child = locals->prong.value == 0
                                         ? node_of(lowering, locals->head.node)->first_child
                                         : node_of(lowering, locals->prong)->next_sibling;
    while (child.value != 0 && node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_SEQUENCE) {
        child = node_of(lowering, child)->next_sibling;
    }
    return child;
}

static sxml_status continue_streamed_fork(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    streamed_fork_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index prong = next_prong(lowering, locals);
    if (prong.value != 0) {
        const stream_point *point = find_point(lowering, prong);
        if (point == NULL || locals->value_count == STREAM_MAXIMUM_PRONG_COUNT) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        locals->prong = prong;
        const streamed_document_locals initial = {.head = {.lowering = lowering, .node = prong},
                                                  .href = locals->href,
                                                  .is_prong = true,
                                                  .scope = point->document};
        sxml_ir_atom *value = &locals->values[locals->value_count];
        locals->value_count += 1;
        sxml_xpath_lowering_frame *prong_frame = NULL;
        return sxml_xpath_lowering_push_procedure(
            machine,
            &(sxml_xpath_lowering_procedure_call){
                .procedure = &streamed_document_procedure,
                .node = no_ast_node,
                .block = frame->block,
                .is_body = false,
                .destination = {.expression = NULL,
                                .atom = value,
                                .atom_block = frame->block,
                                .focused = NULL},
                .initial_locals = {.bytes = (const uint8_t *)&initial,
                                   .byte_count = sizeof(initial)}},
            &prong_frame);
    }
    sxml_xpath_lowerer *lowerer = machine->state;
    sxml_ir_atom_list list = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit,
        (sxml_ir_atom_view){.atoms = locals->values, .atom_count = locals->value_count}, &list);
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                 .sequence = list},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static const sxml_xpath_lowering_procedure streamed_fork_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("streamed xsl:fork"),
    .step = continue_streamed_fork,
    .locals_size = sizeof(streamed_fork_locals)};

sxml_status sxml_xslt_lowering_call_streamed_fork(sxml_xpath_lowering_machine *machine,
                                                  const sxml_xslt_stream_call *call) {
    const sxml_xslt_lowering *lowering = call->lowering;
    if (lowering->stream_plan == NULL ||
        lowering->stream_plan->document_href.atom_kind == SXML_IR_ATOM_KIND_NONE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const streamed_fork_locals initial = {.head = {.lowering = lowering, .node = call->node},
                                          .href = lowering->stream_plan->document_href,
                                          .prong = {0},
                                          .value_count = 0};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &streamed_fork_procedure,
            .node = no_ast_node,
            .block = call->block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = call->atom,
                            .atom_block = call->block,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
}

/* --------------------------------------------- Streamed merge sources */

/* The step test kinds that select elements only (the last step of a
   streamed merge source: its cursor reads elements, packet 7.6g). */
static bool selects_elements(const sxml_ast *ast, sxml_ast_node_index step) {
    const sxml_ast_node_index test = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = step, .field = SXML_AST_FIELD_TEST});
    if (test.value == 0) {
        return false;
    }
    const sxml_ast_kind kind = sxml_ast_get_node(ast, test)->kind;
    return kind == SXML_AST_KIND_NCNAME || kind == SXML_AST_KIND_PREFIXED_NAME ||
           kind == SXML_AST_KIND_URI_QUALIFIED_NAME || kind == SXML_AST_KIND_WILDCARD ||
           kind == SXML_AST_KIND_ELEMENT_NODE_TYPE;
}

/* The steps of a streamed merge source's select, or false. */
static bool read_merge_path(const sxml_xslt_ast_expression *select, stream_path *path) {
    if (select == NULL ||
        !read_path((path_reading){.ast = select->ast,
                                  .node = no_ast_node,
                                  .is_document_focus = true,
                                  .allows_attribute = false},
                   path) ||
        path->step_count == 0) {
        return false;
    }
    for (uint32_t index = 0; index < path->step_count; index += 1U) {
        if (path->steps[index].axis != SXML_AXIS_CHILD || path->steps[index].predicate.value != 0) {
            return false;
        }
    }
    return selects_elements(select->ast, path->steps[path->step_count - 1U].step);
}

bool sxml_xslt_streams_merge_source(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index source) {
    const sxml_xslt_ast_attribute *streamable =
        attribute_of(lowering, source, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE);
    const sxml_xslt_ast_attribute *sorts =
        attribute_of(lowering, source, SXML_XSLT_AST_ATTRIBUTE_SORT_BEFORE_MERGE);
    if (streamable->value.kind != SXML_XSLT_AST_VALUE_KIND_BOOLEAN ||
        !streamable->value.payload.is_true ||
        !has_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE) ||
        has_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM) ||
        (sorts->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN && sorts->value.payload.is_true) ||
        has_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS) ||
        has_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_TYPE) ||
        sxml_xslt_lowering_validates(
            (sxml_xslt_validation_query){.lowering = lowering, .node = source})) {
        return false;
    }
    stream_path path;
    if (!read_merge_path(select_of(lowering, source), &path)) {
        return false;
    }
    bool calls = false;
    return subtree_calls_window_function(lowering, source, &calls) == SXML_STATUS_OK && !calls;
}

bool sxml_xslt_streams_merge(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index merge) {
    size_t streamed_count = 0;
    sxml_xslt_ast_node_index action = {0};
    for (sxml_xslt_ast_node_index child = node_of(lowering, merge)->first_child; child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        const sxml_xslt_ast_kind kind = node_of(lowering, child)->kind;
        if (kind == SXML_XSLT_AST_KIND_MERGE_SOURCE &&
            sxml_xslt_streams_merge_source(lowering, child)) {
            streamed_count += 1;
        } else if (kind == SXML_XSLT_AST_KIND_MERGE_ACTION) {
            action = child;
        }
    }
    if (streamed_count == 0 || streamed_count > SXML_XSLT_MERGE_MAXIMUM_STREAM_COUNT ||
        action.value == 0) {
        return false;
    }
    bool calls = false;
    return subtree_calls_window_function(lowering, action, &calls) == SXML_STATUS_OK && !calls;
}

sxml_status sxml_xslt_lowering_add_merge_chain(sxml_xpath_lowerer *lowerer,
                                               const sxml_xslt_merge_chain_request *request,
                                               sxml_ir_expression_index *stream) {
    const sxml_xslt_lowering *lowering = request->lowering;
    const sxml_xslt_ast_expression *select = select_of(lowering, request->source);
    stream_path path;
    if (!read_merge_path(select, &path)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xpath_static_context *static_context =
        &lowering->static_contexts[select->static_context.value];
    /* Each step's node test, and the focus of the nodes it selects: the
       last step's is the item focus, the others' new slots. */
    step_lowering steps[STREAM_MAXIMUM_STEP_COUNT];
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 0; status == SXML_STATUS_OK && index < path.step_count; index += 1U) {
        step_lowering *step = &steps[index];
        *step = (step_lowering){.value = {0}, .position = {0}, .predicate = {0}};
        status = sxml_xpath_lowerer_resolve_step_test(
            lowerer,
            &(sxml_xpath_step_test_request){
                .ast = select->ast,
                .static_context = static_context,
                .test = sxml_ast_find_child(select->ast,
                                            (sxml_ast_child_query){.parent = path.steps[index].step,
                                                                   .field = SXML_AST_FIELD_TEST}),
                .axis = SXML_AXIS_CHILD},
            &step->node_test);
        if (status == SXML_STATUS_OK && index + 1U == path.step_count) {
            step->value = request->item_focus.context_value;
            step->position = request->item_focus.context_position;
        } else if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_slot(lowerer, &step->value);
        }
    }
    sxml_ir_expression_index body = request->body;
    for (uint32_t index = path.step_count; status == SXML_STATUS_OK && index > 0; index -= 1U) {
        const step_lowering *step = &steps[index - 1U];
        const bool is_last = index == path.step_count;
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_STREAM,
                .stream = {.origin = sxml_ir_slot_atom(
                               index == 1U ? request->document_focus.context_value
                                           : steps[index - 2U].value),
                           .axis = SXML_AXIS_CHILD,
                           .node_test = step->node_test,
                           .predicate = {0},
                           .focus = {.context_value = step->value,
                                     .context_position = step->position,
                                     .context_size = {0}},
                           .position_counter = {0},
                           .body = body,
                           .flags = is_last ? 0U : (uint32_t)SXML_IR_STREAM_SHALLOW_WINDOWS,
                           .fold = {0}}},
            &body);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_STREAM,
                             .stream = {.origin = sxml_ir_slot_atom(request->uri),
                                        .axis = SXML_AXIS_CHILD,
                                        .node_test = {0},
                                        .predicate = {0},
                                        .focus = request->document_focus,
                                        .position_counter = {0},
                                        .body = body,
                                        .flags = (uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT,
                                        .fold = {0}}},
        stream);
}
