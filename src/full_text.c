/*
 * The optional feature full-text (src/full_text_internal.h; D189, D214):
 * its definition and the contains operation, Full Text 3.0, 4.3
 * (fts:FTContainsExpr): for each search context item, its text without
 * the ignored nodes (fts:reconstruct), tokenized (src/full_text_tokenize.c),
 * then the match program run over the tokens (src/full_text_match.c);
 * true for the first item with a match without StringExcludes.
 *
 * A node's text is its string value: the text node descendants in
 * document order, outside the ignored subtrees, so markup inside a word
 * leaves the word whole. Two text nodes whose parents are not one inside
 * the other (an element ends and another starts between them) are in
 * different paragraphs (4.1: paragraphs are implementation-defined);
 * whitespace-only text nodes do not count, so "<p>a</p> <p>b</p>" has two
 * paragraphs and "<p>x <b>y</b> z</p>" one.
 *
 * Golden file: src/timezone.c (a feature's definition, its operations
 * table and the exported function that names it).
 *
 * Section numbers are of XQuery and XPath Full Text 3.0 (W3C
 * Recommendation, 24 November 2015).
 */
#include "full_text_internal.h"

#include "function_library_core_internal.h"
#include "optional_feature_internal.h"
#include "sxml/tree.h"
#include "sxml/xquery.h"
#include "xdm_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

static const sxml_static_error search_item_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOTY0013"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a search context item of contains text is a function item, which has no string "
        "value (Full Text 3.0, 4.3)")};

static const sxml_static_error ignore_item_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an item of the ignore option is not a node (Full Text 3.0, 3.7)")};

/* -------------------------------------------------- Ignored nodes */

/* The nodes of "without content" (3.7), borrowed from the argument. */
typedef struct ignored_nodes {
    sxml_node_reference *nodes;
    size_t node_count;
} ignored_nodes;

static bool is_ignored(const ignored_nodes *ignored, sxml_node_reference node) {
    for (size_t node_index = 0; node_index < ignored->node_count; node_index += 1) {
        if (sxml_node_is_same_node(ignored->nodes[node_index], node)) {
            return true;
        }
    }
    return false;
}

/* Reads the nodes of the ignore argument (its other items cannot be
   ignored and are skipped). */
static sxml_status read_ignored(const sxml_function_call *call, sxml_allocator allocator,
                                ignored_nodes *ignored) {
    const sxml_sequence argument = sxml_function_library_get_argument(call, 2);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(argument, call->error_record, &item_count);
    if (status != SXML_STATUS_OK || item_count == 0) {
        return status;
    }
    if (item_count > SIZE_MAX / sizeof(sxml_node_reference)) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = (size_t)item_count * sizeof(sxml_node_reference),
                                  .alignment = alignof(sxml_node_reference)},
        (void **)&ignored->nodes);
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
         item_index += 1) {
        sxml_item item;
        status = sxml_sequence_get_item(
            argument,
            (sxml_item_request){.item_index = item_index, .error_record = call->error_record},
            &item);
        sxml_node_reference node;
        if (status == SXML_STATUS_OK && sxml_item_get_node(item, &node) != SXML_STATUS_OK) {
            /* 3.7: the ignore option's value is a sequence of nodes. */
            status = sxml_function_library_report(call, &ignore_item_error);
        }
        if (status == SXML_STATUS_OK) {
            ignored->nodes[ignored->node_count] = node;
            ignored->node_count += 1;
        }
    }
    return status;
}

/* ------------------------------------------------------ Item text */

/* The text of one search context item and its paragraph breaks. */
typedef struct item_text {
    sxml_allocator allocator;
    sxml_library_text text;
    uint32_t *breaks;
    size_t break_count;
    size_t break_capacity;
} item_text;

static void release_item_text(item_text *text) {
    sxml_function_library_text_release(&text->text);
    sxml_allocator_release(text->allocator, text->breaks);
    text->breaks = NULL;
    text->break_count = 0;
    text->break_capacity = 0;
}

static sxml_status add_break(item_text *text) {
    if (text->text.byte_count > UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (text->break_count == text->break_capacity) {
        const size_t capacity = text->break_capacity == 0 ? 16U : text->break_capacity * 2U;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate(
            text->allocator,
            (sxml_allocation_request){.byte_count = capacity * sizeof(uint32_t),
                                      .alignment = alignof(uint32_t)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (text->break_count != 0) {
            memcpy(memory, text->breaks, text->break_count * sizeof(uint32_t));
        }
        sxml_allocator_release(text->allocator, text->breaks);
        text->breaks = memory;
        text->break_capacity = capacity;
    }
    text->breaks[text->break_count] = (uint32_t)text->text.byte_count;
    text->break_count += 1;
    return SXML_STATUS_OK;
}

static bool is_whitespace_only(sxml_byte_view content) {
    for (size_t byte_index = 0; byte_index < content.byte_count; byte_index += 1) {
        const uint8_t byte = content.bytes[byte_index];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return false;
        }
    }
    return true;
}

/* Whether one of two nodes (by index, in one tree) contains the other or
   is it: index first..last_descendant. */
typedef struct node_extent {
    uint32_t first;
    uint32_t last;
} node_extent;

static bool are_nested(node_extent outer, node_extent inner) {
    return (outer.first <= inner.first && inner.first <= outer.last) ||
           (inner.first <= outer.first && outer.first <= inner.last);
}

/* The extent of the parent of a node. */
static sxml_status parent_extent(sxml_tree *tree, sxml_node_index parent, node_extent *extent) {
    sxml_node_description description;
    const sxml_status status = sxml_node_describe(
        (sxml_node_reference){.tree = tree, .node_index = parent, .namespace_number = 0},
        &description);
    if (status == SXML_STATUS_OK) {
        *extent = (node_extent){.first = parent.index, .last = description.last_descendant.index};
    }
    return status;
}

/* The text node descendants of a document or element node, outside the
   ignored subtrees (fts:reconstruct), with paragraph breaks. */
typedef struct descendant_walk {
    sxml_node_reference root;
    const ignored_nodes *ignored;
} descendant_walk;

static sxml_status append_descendant_text(descendant_walk walk, sxml_node_index last,
                                          item_text *text) {
    sxml_tree *tree = walk.root.tree;
    bool has_previous = false;
    node_extent previous_parent = {0};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_index = walk.root.node_index.index + 1;
         status == SXML_STATUS_OK && node_index <= last.index;) {
        const sxml_node_reference node = {
            .tree = tree, .node_index = {.index = node_index}, .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (is_ignored(walk.ignored, node)) {
            node_index = description.last_descendant.index + 1;
            continue;
        }
        node_index += 1;
        if (description.node_kind != SXML_NODE_KIND_TEXT) {
            continue;
        }
        sxml_byte_view content;
        status = sxml_node_get_content(node, &content);
        if (status != SXML_STATUS_OK || is_whitespace_only(content)) {
            if (status == SXML_STATUS_OK) {
                status = sxml_function_library_text_append(&text->text, content);
            }
            continue;
        }
        node_extent parent = {0};
        status = parent_extent(tree, description.parent, &parent);
        if (status == SXML_STATUS_OK && has_previous && !are_nested(previous_parent, parent)) {
            status = add_break(text);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&text->text, content);
        }
        previous_parent = parent;
        has_previous = true;
    }
    return status;
}

/* The text of one search context item; outcome->is_true when the item
   itself is an ignored node (it then has none, fts:reconstruct's empty
   sequence). */
typedef struct item_outcome {
    bool is_true;
} item_outcome;

typedef struct item_request {
    const sxml_function_call *call;
    sxml_item item;
    const ignored_nodes *ignored;
} item_request;

static sxml_status make_item_text(item_request request, item_text *text,
                                  item_outcome *is_ignored_item) {
    is_ignored_item->is_true = false;
    const sxml_item_kind kind = sxml_item_get_kind(request.item);
    if (kind == SXML_ITEM_KIND_ATOMIC) {
        return sxml_function_library_text_append_atomic(&text->text, request.item);
    }
    sxml_node_reference node;
    if (kind != SXML_ITEM_KIND_NODE || sxml_item_get_node(request.item, &node) != SXML_STATUS_OK) {
        return sxml_function_library_report(request.call, &search_item_error);
    }
    if (is_ignored(request.ignored, node)) {
        is_ignored_item->is_true = true;
        return SXML_STATUS_OK;
    }
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.node_kind == SXML_NODE_KIND_DOCUMENT ||
        description.node_kind == SXML_NODE_KIND_ELEMENT) {
        return append_descendant_text(
            (descendant_walk){.root = node, .ignored = request.ignored},
            description.last_descendant, text);
    }
    sxml_byte_view content;
    status = sxml_node_get_content(node, &content);
    return status == SXML_STATUS_OK ? sxml_function_library_text_append(&text->text, content)
                                    : status;
}

/* --------------------------------------------------------- contains */

/* What one search context item gives: whether it matches, and its score. */
typedef struct match_outcome {
    bool is_match;
    double score;
} match_outcome;

/* Whether one search context item matches. */
typedef struct item_match {
    item_request item;
    sxml_item program;
} item_match;

/*
 * The score of an item (2.3: implementation-dependent, within [0, 1];
 * glossary `full_text_score`): w / (w + 1 + log2(1 + n)), where w is the
 * weight of its matches without StringExcludes (each 1, times the weights
 * of the selections it passed, averaged over the operands of ftand: a
 * term frequency) and n its token count, so more and heavier matches
 * score higher and longer texts lower, as BM25 saturates and normalizes
 * term frequency; there is no document frequency in one evaluation. 0
 * without a match.
 */
static double item_score(double weight, size_t token_count) {
    if (!(weight > 0.0)) {
        return 0.0;
    }
    return weight / (weight + 1.0 + log2(1.0 + (double)token_count));
}

static sxml_status match_item(item_match match, match_outcome *outcome) {
    const item_request request = match.item;
    *outcome = (match_outcome){.is_match = false, .score = 0.0};
    const sxml_allocator allocator = sxml_item_heap_as_allocator(request.call->item_heap);
    item_text text = {.allocator = allocator, .text = {.item_heap = request.call->item_heap}};
    item_outcome is_ignored_item = {.is_true = false};
    sxml_status status = make_item_text(request, &text, &is_ignored_item);
    sxml_full_text_token_list tokens = {.allocator = allocator};
    if (status == SXML_STATUS_OK && !is_ignored_item.is_true) {
        status = sxml_full_text_tokenize(
            &(sxml_full_text_source){.text = sxml_function_library_text_view(&text.text),
                                     .paragraph_breaks = text.breaks,
                                     .paragraph_break_count = text.break_count,
                                     .keeps_wildcards = false},
            &tokens);
    }
    sxml_full_text_run_outcome run_outcome = {.is_match = false, .weight = 0.0};
    if (status == SXML_STATUS_OK && !is_ignored_item.is_true) {
        status = sxml_full_text_run(
            &(sxml_full_text_run_request){
                .call = request.call,
                .program = match.program,
                .search = {.text = sxml_function_library_text_view(&text.text),
                           .tokens = tokens.tokens,
                           .token_count = tokens.token_count}},
            &run_outcome);
    }
    outcome->is_match = run_outcome.is_match;
    outcome->score = run_outcome.is_match ? item_score(run_outcome.weight, tokens.token_count)
                                          : 0.0;
    sxml_full_text_token_list_release(&tokens);
    release_item_text(&text);
    return status;
}

/* fts:FTContainsExpr over every search context item (4.3): whether one
   matches (the first stops the search when only that is wanted), and the
   highest score of one. */
typedef struct contains_request {
    const sxml_function_call *call;
    bool is_scored;
} contains_request;

static sxml_status evaluate_contains(contains_request request, match_outcome *outcome) {
    const sxml_function_call *call = request.call;
    *outcome = (match_outcome){.is_match = false, .score = 0.0};
    const sxml_sequence search = sxml_function_library_get_argument(call, 0);
    sxml_optional_item program;
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &program);
    if (status == SXML_STATUS_OK && !program.has_item) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    ignored_nodes ignored = {.nodes = NULL, .node_count = 0};
    if (status == SXML_STATUS_OK) {
        status = read_ignored(call, allocator, &ignored);
    }
    for (uint64_t item_index = 0;
         status == SXML_STATUS_OK && (request.is_scored || !outcome->is_match);
         item_index += 1) {
        sxml_item item;
        status = sxml_sequence_get_item(
            search,
            (sxml_item_request){.item_index = item_index, .error_record = call->error_record},
            &item);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            status = SXML_STATUS_OK;
            break;
        }
        match_outcome item_found = {.is_match = false, .score = 0.0};
        if (status == SXML_STATUS_OK) {
            status = match_item(
                (item_match){.item = {.call = call, .item = item, .ignored = &ignored},
                             .program = program.item},
                &item_found);
        }
        outcome->is_match = outcome->is_match || item_found.is_match;
        if (item_found.score > outcome->score) {
            outcome->score = item_found.score;
        }
    }
    sxml_allocator_release(allocator, ignored.nodes);
    return status;
}

static sxml_status contains(const sxml_function_call *call, sxml_sequence *result) {
    match_outcome outcome;
    const sxml_status status =
        evaluate_contains((contains_request){.call = call, .is_scored = false}, &outcome);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = outcome.is_match});
    }
    return status;
}

/* The score of "E contains text S" (2.3): the highest of its items. */
static sxml_status score_contains(const sxml_function_call *call, sxml_sequence *result) {
    match_outcome outcome;
    const sxml_status status =
        evaluate_contains((contains_request){.call = call, .is_scored = true}, &outcome);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_double(outcome.score));
    }
    return status;
}

/* ---------------------------------------------------------- Definition */

/* Defined by the generated parser of grammars/xpath_full_text (D12,
   D235). */
const struct TSLanguage *
tree_sitter_xpath_full_text(void); // NOLINT(readability-identifier-naming)

/* XPath with Full Text (Appendix B; D235): the XPath front end's tables
   and hooks, and Full Text's as the extension. Only the feature links its
   grammar, so the core sxml.wasm keeps grammars/xpath alone. */
static const sxml_ast_parser_part xpath_parser_part = {
    .language = tree_sitter_xpath_full_text,
    .root_node_type = SXML_BYTE_VIEW_INITIALIZER("xpath"),
    .node_types = {.entries = NULL, .entry_count = 0},
    .fields = {.entries = NULL, .entry_count = 0},
    .unnamed_children = {.entries = NULL, .entry_count = 0},
    /* The XPath front end's descriptions. */
    .syntax_error_description = {.bytes = NULL, .byte_count = 0},
    .missing_token_description = {.bytes = NULL, .byte_count = 0},
    .apply_token = NULL,
    .describe_payload = NULL,
    .finish_node = NULL,
    .extension = &sxml_full_text_parser_part,
};

static const sxml_full_text_operations full_text_operations = {
    .contains = contains,
    .score_contains = score_contains,
    .check_rules = sxml_full_text_check_rules,
    .lower_step = sxml_full_text_lower_step,
    .xpath_parser_part = &xpath_parser_part,
    .call_for_score = sxml_xquery_call_full_text_for_score,
};

const sxml_optional_feature_definition sxml_full_text_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_FULL_TEXT,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("full-text"),
    .operations = &full_text_operations,
    .finalize_state = NULL,
    .library_functions = NULL,
};

sxml_optional_feature sxml_full_text_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_full_text_feature_definition};
}
