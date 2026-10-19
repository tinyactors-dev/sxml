/*
 * Streamed evaluation in XSLT: the lowering (src/xslt_lower_internal.h,
 * "Streamed evaluation"; XSLT 4.0, 18.1 and 19; D219, after D212).
 *
 * A streamed body (glossary `stream_point`, `stream_window`): the
 * sequence constructor of xsl:source-document with streamable="yes", and
 * of the xsl:for-each instructions in it that select streamed nodes. Its
 * instructions (inside literal result elements, which only wrap their
 * content) are each either motionless in the focus of the streamed node
 * (XSLT 4.0, 19: they read its name, attributes and ancestors, never its
 * content; the sweep is 7.6a's, src/xslt_streamability_internal.h) or a
 * stream point, of which a streamed body has at most one: an xsl:for-each
 * whose select is a downward path from the focus,
 *
 *     step ("/" step)*            step: child::T, T, descendant::T
 *     "//" T at the start, or as the last step (descendant::T)
 *
 * each step with at most one predicate that is motionless, the descendant
 * step last, and whose body is at most consuming. Such a body is streamed:
 * xsl:source-document becomes STREAM with OPENS_DOCUMENT over its href,
 * and each stream point a chain of STREAMs, one per step, the body of
 * each the STREAM of the next (src/ir_internal.h; the path's position in
 * a counter slot the chain shares). A stream point whose own body is
 * streamed gets shallow windows: the selected element's content is read
 * by the body's stream point as it passes; otherwise each selected node's
 * window holds its subtree (the body consumes it, 19.8.1), built while
 * its events pass.
 *
 * Everything else is lowered as before, unstreamed: an xsl:source-document
 * whose body has another shape reads the whole document (fn:doc), which
 * gives the same results (19.10). Not streamed yet: accumulators (the
 * stylesheet declares one), validation and type on xsl:source-document,
 * xsl:apply-templates in streamable modes, xsl:iterate, xsl:fork,
 * xsl:merge, aggregates over a streamed path (count(//x)), and bodies
 * that read fn:has-children, fn:path or fn:last (motionless by 19.8.9,
 * but they need what a shallow window lacks).
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

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* -------------------------------------------------------------- Plans */

/* A stream point found in a streamed body. */
typedef struct stream_point {
    sxml_xslt_ast_node_index node;
    /* Its body is streamed: shallow windows for its items. */
    bool is_shallow;
} stream_point;

/* A body to plan: the sequence constructor of node; point_index + 1 is
   its stream point (0 for the xsl:source-document's own body). */
typedef struct stream_body {
    sxml_xslt_ast_node_index node;
    size_t point_number;
} stream_body;

struct sxml_xslt_stream_plan {
    sxml_allocator allocator;
    stream_point *points;
    size_t point_count;
    size_t point_capacity;
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
    *new_plan = (sxml_xslt_stream_plan){
        .allocator = allocator, .points = NULL, .point_count = 0, .point_capacity = 0};
    *plan = new_plan;
    return SXML_STATUS_OK;
}

void sxml_xslt_release_stream_plan(sxml_xslt_stream_plan *plan) {
    if (plan == NULL) {
        return;
    }
    sxml_allocator_release(plan->allocator, plan->points);
    sxml_allocator_release(plan->allocator, plan);
}

static sxml_status add_point(sxml_xslt_stream_plan *plan, stream_point point) {
    if (plan->point_count == plan->point_capacity) {
        const size_t capacity = plan->point_capacity == 0 ? 8U : plan->point_capacity * 2U;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate_array(
            plan->allocator,
            (sxml_array_allocation_request){.element_count = capacity,
                                            .element_size = sizeof(stream_point),
                                            .element_alignment = alignof(stream_point)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (plan->point_count > 0) {
            memcpy(memory, plan->points, plan->point_count * sizeof(stream_point));
        }
        sxml_allocator_release(plan->allocator, plan->points);
        plan->points = memory;
        plan->point_capacity = capacity;
    }
    plan->points[plan->point_count] = point;
    plan->point_count += 1;
    return SXML_STATUS_OK;
}

static const stream_point *find_point(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node) {
    const sxml_xslt_stream_plan *plan = lowering->stream_plan;
    if (plan == NULL) {
        return NULL;
    }
    for (size_t point_index = 0; point_index < plan->point_count; point_index += 1) {
        if (plan->points[point_index].node.value == node.value) {
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

/* Whether ast calls one of the functions a stream window cannot answer
   (fn:has-children, fn:path; fn:last and fn:function-lookup, which read
   the size of the focus a streamed selection has not), by local name. */
static bool calls_window_function(const sxml_ast *ast) {
    static const char *const names[] = {"has-children", "path", "last", "function-lookup"};
    for (uint32_t index = 1; index < ast->node_count; index += 1) {
        const sxml_ast_node *node = sxml_ast_get_node(ast, (sxml_ast_node_index){.value = index});
        if (node->kind != SXML_AST_KIND_FUNCTION_CALL &&
            node->kind != SXML_AST_KIND_NAMED_FUNCTION_REFERENCE) {
            continue;
        }
        const sxml_ast_node_index name = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = {.value = index}, .field = SXML_AST_FIELD_NAME});
        const sxml_byte_view local_name =
            sxml_ast_get_text(ast, sxml_ast_get_node(ast, name)->payload.name.local_name);
        for (size_t name_index = 0; name_index < sizeof(names) / sizeof(names[0]);
             name_index += 1) {
            const size_t length = strlen(names[name_index]);
            if (local_name.byte_count == length &&
                memcmp(local_name.bytes, names[name_index], length) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Whether an XSLT subtree (node and its descendants) has an expression
   calling such a function. A walk with an explicit stack (D71). */
static sxml_status subtree_calls_window_function(const sxml_xslt_lowering *lowering,
                                                 sxml_xslt_ast_node_index root, bool *calls) {
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
        /* Template and attribute set calls pass the focus size on. */
        *calls = entry->kind == SXML_XSLT_AST_KIND_CALL_TEMPLATE ||
                 (content != NULL && calls_window_function(content->ast));
        for (uint32_t offset = 0; !*calls && offset < entry->attributes.attribute_count;
             offset += 1) {
            const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
                lowering->ast, (sxml_xslt_ast_attribute_index){
                                   .value = entry->attributes.first_attribute_index + offset});
            const sxml_xslt_ast_expression *expression =
                expression_of_value(lowering, attribute->value);
            *calls = attribute->kind == SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS ||
                     (expression != NULL && calls_window_function(expression->ast));
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
            .requires_grounded = question.requires_grounded},
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

/* The steps of a streamed path (see the top of the file), or false. At
   the document node, "/" and "//" at the start mean the focus. */
typedef struct path_reading {
    const sxml_ast *ast;
    bool is_document_focus;
} path_reading;

static bool read_path(path_reading reading, stream_path *path) {
    const sxml_ast *ast = reading.ast;
    path->step_count = 0;
    sxml_ast_node_index node = sxml_ast_find_child(
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
                                              .requires_grounded = true},
                             motionless);
    }
    sxml_status status =
        is_motionless(lowering,
                      (sweep_question){.instruction = question.node,
                                       .assesses_content = false,
                                       .expression = NULL,
                                       .expression_node = no_ast_node,
                                       .is_document_focus = question.is_document_focus,
                                       .requires_grounded = true},
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

static sxml_status plan_one_body(const body_planning *planning, bool *is_streamed) {
    const sxml_xslt_lowering *lowering = planning->lowering;
    const stream_body body = planning->list->bodies[planning->body_index];
    const bool is_document_focus = body.point_number == 0;
    *is_streamed = true;
    size_t point_count = 0;
    /* The siblings still to visit, one chain per open wrapping element. */
    sxml_xslt_ast_node_index pending[64];
    size_t pending_count = 0;
    pending[pending_count] = sxml_xslt_lowering_first_constructor_child(lowering->ast, body.node);
    pending_count += 1;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && *is_streamed && pending_count > 0) {
        const sxml_xslt_ast_node_index node = pending[pending_count - 1U];
        if (node.value == 0) {
            pending_count -= 1;
            continue;
        }
        pending[pending_count - 1U] = node_of(lowering, node)->next_sibling;
        const point_question question = {.node = node, .is_document_focus = is_document_focus};
        bool is_point = false;
        status = is_eligible_point(lowering, question, &is_point);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (is_point) {
            point_count += 1;
            status =
                add_point(lowering->stream_plan, (stream_point){.node = node, .is_shallow = false});
            if (status == SXML_STATUS_OK) {
                status =
                    push_body(lowering, planning->list,
                              (stream_body){.node = node,
                                            .point_number = lowering->stream_plan->point_count});
            }
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
        has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS) ||
        sxml_xslt_has_accumulators(lowering) ||
        sxml_xslt_lowering_has_xslt_1_behaviour(lowering, node)) {
        return SXML_STATUS_OK;
    }
    const size_t first_point = plan->point_count;
    body_list list = {.bodies = NULL, .count = 0, .capacity = 0};
    sxml_status status = push_body(lowering, &list, (stream_body){.node = node, .point_number = 0});
    for (size_t body_index = 0; status == SXML_STATUS_OK && body_index < list.count;
         body_index += 1) {
        bool is_body_streamed = false;
        status = plan_one_body(
            &(body_planning){.lowering = lowering, .list = &list, .body_index = body_index},
            &is_body_streamed);
        const size_t point_number = list.bodies[body_index].point_number;
        if (point_number == 0) {
            *is_streamed = is_body_streamed;
            if (!is_body_streamed) {
                break;
            }
        } else {
            plan->points[point_number - 1U].is_shallow = is_body_streamed;
        }
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, list.bodies);
    if (status != SXML_STATUS_OK || !*is_streamed) {
        /* Not streamed: its points are lowered as before. */
        plan->point_count = first_point;
        *is_streamed = false;
    }
    return status;
}

/* --------------------------------------------- Lowering a stream point */

/* The slots of one step's focus, and its node test. */
typedef struct step_lowering {
    sxml_ir_slot value;
    sxml_ir_slot position;
    sxml_type_identifier node_test;
    sxml_xpath_lowering_focus predicate_focus;
    sxml_ir_expression_index predicate;
} step_lowering;

typedef struct stream_point_locals {
    sxml_xslt_instruction_locals head;
    sxml_xpath_lowering_block *block;
    const sxml_xslt_ast_expression *select;
    stream_path path;
    step_lowering steps[STREAM_MAXIMUM_STEP_COUNT];
    uint32_t next_predicate;
    sxml_ir_atom origin;
    sxml_ir_slot counter;
    bool is_shallow;
    sxml_xpath_lowering_focus item_focus;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
} stream_point_locals;

enum {
    SXML_STREAM_POINT_STEP_START = 0,
    SXML_STREAM_POINT_STEP_PREDICATES = 1,
    SXML_STREAM_POINT_STEP_BODY = 2,
    SXML_STREAM_POINT_STEP_FINISH = 3,
};

/* Reads the path, resolves its node tests, makes the slots. */
static sxml_status start_stream_point(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    stream_point_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const stream_point *point = find_point(lowering, locals->head.node);
    locals->select = select_of(lowering, locals->head.node);
    if (point == NULL || locals->select == NULL ||
        !read_path((path_reading){.ast = locals->select->ast, .is_document_focus = true},
                   &locals->path)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    locals->is_shallow = point->is_shallow;
    const sxml_xpath_static_context *static_context =
        &lowering->static_contexts[locals->select->static_context.value];
    sxml_status status = sxml_xpath_lowerer_read_focus(
        lowerer,
        (sxml_xpath_lowering_focus_read){.block = frame->block,
                                         .node = no_ast_node,
                                         .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
        &locals->origin);
    for (uint32_t index = 0; status == SXML_STATUS_OK && index < locals->path.step_count;
         index += 1U) {
        step_lowering *step = &locals->steps[index];
        const stream_step *read = &locals->path.steps[index];
        status = sxml_xpath_lowerer_resolve_step_test(
            lowerer,
            &(sxml_xpath_step_test_request){
                .ast = locals->select->ast,
                .static_context = static_context,
                .test = sxml_ast_find_child(
                    locals->select->ast,
                    (sxml_ast_child_query){.parent = read->step, .field = SXML_AST_FIELD_TEST}),
                .axis = read->axis},
            &step->node_test);
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
    if (status == SXML_STATUS_OK && locals->path.step_count > 1U) {
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
                    .block = frame->block,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                   .sequence = list}},
                &counter);
        }
        if (status == SXML_STATUS_OK && counter.atom_kind != SXML_IR_ATOM_KIND_SLOT) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        locals->counter = counter.slot;
    }
    return status;
}

/* Lowers the next predicate as a body in its step's focus. */
static sxml_status call_next_predicate(sxml_xpath_lowering_machine *machine,
                                       stream_point_locals *locals, bool *has_called) {
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    *has_called = false;
    while (locals->next_predicate < locals->path.step_count) {
        const uint32_t index = locals->next_predicate;
        locals->next_predicate += 1U;
        if (locals->path.steps[index].predicate.value == 0) {
            continue;
        }
        *has_called = true;
        return sxml_xpath_lowering_call_embedded(
            machine,
            &(sxml_xpath_lowering_embedded_call){
                .embedding = {.ast = locals->select->ast,
                              .node = locals->path.steps[index].predicate,
                              .static_context =
                                  &lowering->static_contexts[locals->select->static_context.value],
                              .focus = &locals->steps[index].predicate_focus},
                .block = NULL,
                .is_body = true,
                .destination = {.expression = &locals->steps[index].predicate,
                                .atom = NULL,
                                .atom_block = NULL,
                                .focused = NULL}});
    }
    return SXML_STATUS_OK;
}

/* The chain of STREAMs, the last first. */
static sxml_status finish_stream_point(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    stream_point_locals *locals = frame->locals;
    for (uint32_t index = 0; index < locals->path.step_count; index += 1U) {
        if (locals->steps[index].predicate_focus.parts_read.context_size.value != 0) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if (locals->item_focus.parts_read.context_size.value != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_expression_index body = {0};
    sxml_ir_expression_index result = {0};
    sxml_status status = sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = locals->body},
        &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer,
            (sxml_xpath_lowering_block_result){.block = locals->body_block, .result = result},
            &body);
    }
    for (uint32_t index = locals->path.step_count; status == SXML_STATUS_OK && index > 0;
         index -= 1U) {
        const step_lowering *step = &locals->steps[index - 1U];
        const bool is_last = index == locals->path.step_count;
        const sxml_ir_atom origin =
            index == 1U ? locals->origin : sxml_ir_slot_atom(locals->steps[index - 2U].value);
        const uint32_t flags =
            !is_last || locals->is_shallow ? (uint32_t)SXML_IR_STREAM_SHALLOW_WINDOWS : 0U;
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_STREAM,
                .stream = {.origin = origin,
                           .axis = locals->path.steps[index - 1U].axis,
                           .node_test = step->node_test,
                           .predicate = step->predicate,
                           .focus = {.context_value = step->value,
                                     .context_position = step->position,
                                     .context_size = {0}},
                           .position_counter = is_last && locals->path.step_count > 1U
                                                   ? locals->counter
                                                   : (sxml_ir_slot){0},
                           .body = body,
                           .flags = flags}},
            &body);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, body);
}

static sxml_status continue_stream_point(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    stream_point_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == SXML_STREAM_POINT_STEP_START) {
        frame->step = SXML_STREAM_POINT_STEP_PREDICATES;
        status = start_stream_point(machine, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (frame->step == SXML_STREAM_POINT_STEP_PREDICATES) {
        bool has_called = false;
        status = call_next_predicate(machine, locals, &has_called);
        if (status != SXML_STATUS_OK || has_called) {
            return status;
        }
        frame->step = SXML_STREAM_POINT_STEP_BODY;
    }
    if (frame->step == SXML_STREAM_POINT_STEP_BODY) {
        frame->step = SXML_STREAM_POINT_STEP_FINISH;
        const step_lowering *last = &locals->steps[locals->path.step_count - 1U];
        locals->item_focus =
            (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
                                        .slots = {.context_value = last->value,
                                                  .context_position = last->position,
                                                  .context_size = {0}}};
        locals->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
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
                         .focus = &locals->item_focus});
    }
    return finish_stream_point(machine, frame);
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

/* ------------------------------------- Lowering a streamed document body */

typedef struct streamed_document_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom href;
    sxml_xpath_lowering_focus document_focus;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
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
    if (locals->document_focus.parts_read.context_size.value != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_expression_index result = {0};
    sxml_ir_expression_index body = {0};
    sxml_status status = sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = locals->body},
        &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer,
            (sxml_xpath_lowering_block_result){.block = locals->body_block, .result = result},
            &body);
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
                                            .flags = (uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT}},
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
