/*
 * Source code as XML (src/source_code_internal.h; packet 7.22, D285):
 * a tree-sitter syntax tree as tree builder events, with the captures of
 * the grammar's highlights query as attributes.
 *
 * Three steps, each bounded by the request's step meter:
 * 1. tree-sitter parses the text (D12); its progress callback charges the
 *    bytes read and cancels the parse when the meter refuses.
 * 2. With highlights, the highlights query runs over the tree. Each match
 *    whose text predicates hold (eq?, match?, any-of? and their
 *    negations; tree-sitter leaves predicates to the host) records its
 *    captures; for a node that several patterns capture, the last
 *    pattern in the query wins, as the grammars' highlight tests expect
 *    (their queries start with "(identifier) @variable" and refine it
 *    later; D285). Captures whose names start with "_" only serve
 *    predicates. The records are sorted by node, so the walk finds a
 *    node's highlight by binary search.
 * 3. One loop walks the tree with a TSTreeCursor, depth first, and keeps
 *    which open nodes made elements on an explicit stack (D71). Text is
 *    written by byte offset: whatever lies between the end of what was
 *    written and the start of the next node is written first, so no
 *    byte of the source is lost or written twice.
 *
 * Golden files: src/json.c (the cursor walk, tree-sitter's lifetime) and
 * src/ixml.c (building a document from a parse).
 */
#include "source_code_internal.h"

#include "memory_internal.h"
#include "regex_internal.h"
#include "sxml/string.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ Languages */

const sxml_source_code_language *sxml_source_code_find_language(sxml_byte_view name) {
    for (size_t language_index = 0; language_index < sxml_source_code_languages.language_count;
         language_index += 1) {
        const sxml_source_code_language *language =
            &sxml_source_code_languages.languages[language_index];
        if (language->language_name.byte_count == name.byte_count &&
            (name.byte_count == 0 ||
             memcmp(language->language_name.bytes, name.bytes, name.byte_count) == 0)) {
            return language;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- Steps */

/* Steps are charged in batches, so the meter is not called per node. */
#define SXML_SOURCE_CODE_STEP_BATCH 256U

/* The steps counted and not yet charged, and the first refusal. */
typedef struct step_account {
    sxml_source_code_step_meter meter;
    uint64_t pending_step_count;
    sxml_status status;
} step_account;

static sxml_status settle_steps(step_account *account) {
    if (account->status == SXML_STATUS_OK && account->pending_step_count > 0) {
        account->status =
            account->meter.operations == NULL
                ? SXML_STATUS_OK
                : account->meter.operations->charge(account->meter.state,
                                                    account->pending_step_count);
        account->pending_step_count = 0;
    }
    return account->status;
}

/* Counts step_count steps; false once the meter has refused. */
static bool count_steps(step_account *account, uint64_t step_count) {
    account->pending_step_count += step_count;
    if (account->pending_step_count >= SXML_SOURCE_CODE_STEP_BATCH) {
        (void)settle_steps(account);
    }
    return account->status == SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Parse */

typedef struct source_input {
    sxml_byte_view text;
} source_input;

/* TSInput.read: the rest of the text in one piece. Its four parameters
   are tree-sitter's (api.h, TSInput). */
// NOLINTNEXTLINE(readability-function-size)
static const char *read_source(void *payload, uint32_t byte_index, TSPoint position,
                               uint32_t *bytes_read) {
    (void)position;
    const source_input *input = payload;
    if (byte_index >= input->text.byte_count) {
        *bytes_read = 0;
        return "";
    }
    *bytes_read = (uint32_t)(input->text.byte_count - byte_index);
    return (const char *)input->text.bytes + byte_index;
}

typedef struct parse_progress {
    step_account *account;
    uint32_t last_byte_offset;
} parse_progress;

/* TSParseOptions.progress_callback: one step per call and per byte read;
   true cancels the parse. */
static bool report_parse_progress(TSParseState *state) {
    parse_progress *progress = state->payload;
    uint64_t step_count = 1;
    if (state->current_byte_offset > progress->last_byte_offset) {
        step_count += state->current_byte_offset - progress->last_byte_offset;
        progress->last_byte_offset = state->current_byte_offset;
    }
    return !count_steps(progress->account, step_count);
}

/* Parses request's text into *tree (NULL on failure). */
static sxml_status parse_tree(const sxml_source_code_request *request, step_account *account,
                              TSTree **tree) {
    *tree = NULL;
    TSParser *parser = ts_parser_new();
    if (parser == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    if (!ts_parser_set_language(parser, request->language->get_grammar())) {
        ts_parser_delete(parser);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    source_input input = {.text = request->source_text};
    parse_progress progress = {.account = account, .last_byte_offset = 0};
    *tree = ts_parser_parse_with_options(
        parser, NULL,
        (TSInput){.payload = &input,
                  .read = read_source,
                  .encoding = TSInputEncodingUTF8,
                  .decode = NULL},
        (TSParseOptions){.payload = &progress, .progress_callback = report_parse_progress});
    ts_parser_delete(parser);
    if (*tree == NULL) {
        return account->status != SXML_STATUS_OK ? account->status : SXML_STATUS_OUT_OF_MEMORY;
    }
    return settle_steps(account);
}

/* ----------------------------------------------------------- Highlights */

/* One capture of a highlights match: the node (TSNode.id, unique in its
   tree), the pattern and the capture name. */
typedef struct highlight_capture {
    uintptr_t node_identity;
    uint32_t pattern_index;
    uint32_t capture_index;
} highlight_capture;

/* A compiled regex of a match? predicate, by its string's identifier. */
typedef struct predicate_regex {
    sxml_regex *regex;
    sxml_regex_matcher *matcher;
    /* Compiled, or found not to compile (then the predicate fails). */
    bool is_tried;
} predicate_regex;

/* The highlights of one tree. */
typedef struct highlighter {
    const sxml_source_code_request *request;
    step_account *account;
    TSQuery *query;
    /* highlight_capture, sorted by node, one per node. */
    sxml_xml_array captures;
    predicate_regex *regexes;
    uint32_t regex_count;
    /* By capture index: true for a name that is a highlight. */
    bool *is_highlight_capture;
} highlighter;

static sxml_byte_view node_text(const highlighter *highlighter, TSNode node) {
    const sxml_byte_view text = highlighter->request->source_text;
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (end > text.byte_count) {
        end = (uint32_t)text.byte_count;
    }
    if (start > end) {
        start = end;
    }
    return (sxml_byte_view){.bytes = text.bytes + start, .byte_count = end - start};
}

static bool byte_views_equal(sxml_byte_view first, sxml_byte_view second) {
    return first.byte_count == second.byte_count &&
           (first.byte_count == 0 || memcmp(first.bytes, second.bytes, first.byte_count) == 0);
}

static sxml_byte_view query_string(const highlighter *highlighter, uint32_t value_id) {
    uint32_t length = 0;
    const char *value = ts_query_string_value_for_id(highlighter->query, value_id, &length);
    return (sxml_byte_view){.bytes = (const uint8_t *)value, .byte_count = length};
}

/* The regex of string value_id, compiled at its first use; NULL when it
   does not compile as an XPath regex (D285: tree-sitter queries write
   Rust regexes, and the common subset means the same). */
static sxml_status find_regex(highlighter *highlighter, uint32_t value_id,
                              predicate_regex **found) {
    *found = NULL;
    if (value_id >= highlighter->regex_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    predicate_regex *entry = &highlighter->regexes[value_id];
    if (!entry->is_tried) {
        entry->is_tried = true;
        sxml_string regex_text;
        sxml_status status = sxml_string_from_utf8(query_string(highlighter, value_id), &regex_text);
        const sxml_allocator allocator = highlighter->request->allocator;
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_create(
                allocator,
                &(sxml_regex_configuration){.regex_text = regex_text,
                                            .flags = {.is_dot_all = false},
                                            .maximum_instruction_count = 0,
                                            .maximum_group_depth = 0,
                                            .stack_allocator = {.state = NULL, .operations = NULL},
                                            .error_record = NULL},
                &entry->regex);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_matcher_create(
                allocator,
                &(sxml_regex_matcher_configuration){.regex = entry->regex,
                                                    .backtrack_allocator = allocator,
                                                    .initial_backtrack_entry_count = 0,
                                                    .maximum_backtrack_byte_count = 0,
                                                    .maximum_step_count = 0},
                &entry->matcher);
        }
        if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_INVALID_UTF8) {
            status = SXML_STATUS_OK;
        }
        if (status != SXML_STATUS_OK || entry->matcher == NULL) {
            sxml_regex_matcher_destroy(entry->matcher);
            sxml_regex_destroy(entry->regex);
            entry->matcher = NULL;
            entry->regex = NULL;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (entry->matcher != NULL) {
        *found = entry;
    }
    return SXML_STATUS_OK;
}

/* A regex search: the regex and the text it searches. */
typedef struct regex_search {
    predicate_regex *entry;
    sxml_byte_view text;
} regex_search;

/* Whether the regex matches somewhere in the text (Rust's is_match). */
static sxml_status regex_matches(highlighter *highlighter, regex_search search, bool *matches) {
    predicate_regex *entry = search.entry;
    sxml_string input;
    sxml_status status = sxml_string_from_utf8(search.text, &input);
    sxml_regex_search_result found = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_find(
            entry->matcher,
            &(sxml_regex_search_request){.regex_input = input, .start_byte_offset = 0}, &found);
    }
    if (status == SXML_STATUS_OK &&
        !count_steps(highlighter->account, sxml_regex_matcher_get_step_count(entry->matcher))) {
        status = highlighter->account->status;
    }
    *matches = status == SXML_STATUS_OK && found.is_found;
    return status;
}

/* One predicate of a match: its steps (name first), without the DONE. */
typedef struct predicate_call {
    const TSQueryMatch *match;
    const TSQueryPredicateStep *steps;
    uint32_t step_count;
} predicate_call;

/* Whether name is base_text, or base_text after "any-", "not-" or
   "any-not-" (tree-sitter's predicate families). */
static bool has_base(sxml_byte_view name, const char *base_text) {
    const sxml_byte_view base = sxml_byte_view_from_c_string(base_text);
    for (size_t offset = 0; offset <= 8 && offset <= name.byte_count; offset += 4) {
        const sxml_byte_view rest = {.bytes = name.bytes + offset,
                                     .byte_count = name.byte_count - offset};
        if (byte_views_equal(rest, base)) {
            const sxml_byte_view prefix = {.bytes = name.bytes, .byte_count = offset};
            return offset == 0 || byte_views_equal(prefix, SXML_BYTE_VIEW_FROM_LITERAL("any-")) ||
                   byte_views_equal(prefix, SXML_BYTE_VIEW_FROM_LITERAL("not-")) ||
                   byte_views_equal(prefix, SXML_BYTE_VIEW_FROM_LITERAL("any-not-"));
        }
    }
    return false;
}

/* The first node of capture capture_index in the match, if any. */
static bool find_capture_node(const TSQueryMatch *match, uint32_t capture_index, TSNode *node) {
    for (uint16_t index = 0; index < match->capture_count; index += 1) {
        if (match->captures[index].index == capture_index) {
            *node = match->captures[index].node;
            return true;
        }
    }
    return false;
}

/* Whether the one node's text passes the predicate's test (before
   negation): equal to the second argument, matching the regex, or one of
   the strings. */
typedef struct node_test {
    const predicate_call *call;
    sxml_byte_view predicate_name;
    TSNode node;
} node_test;

static sxml_status test_node(highlighter *highlighter, const node_test *test, bool *passes) {
    const predicate_call *call = test->call;
    const sxml_byte_view text = node_text(highlighter, test->node);
    *passes = true;
    if (has_base(test->predicate_name, "eq?")) {
        const TSQueryPredicateStep argument = call->steps[2];
        sxml_byte_view other = {.bytes = NULL, .byte_count = 0};
        if (argument.type == TSQueryPredicateStepTypeCapture) {
            TSNode other_node;
            if (!find_capture_node(call->match, argument.value_id, &other_node)) {
                return SXML_STATUS_OK;
            }
            other = node_text(highlighter, other_node);
        } else {
            other = query_string(highlighter, argument.value_id);
        }
        *passes = byte_views_equal(text, other);
        return SXML_STATUS_OK;
    }
    if (has_base(test->predicate_name, "match?") || has_base(test->predicate_name, "lua-match?")) {
        predicate_regex *entry = NULL;
        const sxml_status status = find_regex(highlighter, call->steps[2].value_id, &entry);
        if (status != SXML_STATUS_OK || entry == NULL) {
            *passes = false;
            return status;
        }
        return regex_matches(highlighter, (regex_search){.entry = entry, .text = text}, passes);
    }
    /* any-of?: one of the strings. */
    *passes = false;
    for (uint32_t step_index = 2; step_index < call->step_count; step_index += 1) {
        if (byte_views_equal(text, query_string(highlighter, call->steps[step_index].value_id))) {
            *passes = true;
        }
    }
    return SXML_STATUS_OK;
}

/* Evaluates one predicate; unknown predicates and directives (#set!)
   hold. */
static sxml_status evaluate_predicate(highlighter *highlighter, const predicate_call *call,
                                      bool *holds) {
    *holds = true;
    if (call->step_count == 0 || call->steps[0].type != TSQueryPredicateStepTypeString) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view name = query_string(highlighter, call->steps[0].value_id);
    /* #is? local / #is-not? local (locals are not tracked, D285: no node
       is a local variable, as when a grammar has no locals query). */
    if (byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("is?")) ||
        byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("is-not?"))) {
        const bool is_local = call->step_count >= 2 &&
                              call->steps[1].type == TSQueryPredicateStepTypeString &&
                              byte_views_equal(query_string(highlighter, call->steps[1].value_id),
                                               SXML_BYTE_VIEW_FROM_LITERAL("local"));
        *holds = !is_local || name.byte_count == 7;
        return SXML_STATUS_OK;
    }
    const bool is_text_predicate =
        has_base(name, "eq?") || has_base(name, "match?") || has_base(name, "lua-match?") ||
        byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("any-of?")) ||
        byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("not-any-of?"));
    if (!is_text_predicate || call->step_count < 3 ||
        call->steps[1].type != TSQueryPredicateStepTypeCapture) {
        return SXML_STATUS_OK;
    }
    const bool is_any_of = byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("any-of?")) ||
                           byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("not-any-of?"));
    const bool is_negated =
        is_any_of ? name.bytes[0] == 'n'
                  : (name.byte_count > 4 && memcmp(name.bytes, "not-", 4) == 0) ||
                        (name.byte_count > 8 && memcmp(name.bytes, "any-not-", 8) == 0);
    const bool is_any = !is_any_of && name.byte_count > 4 && memcmp(name.bytes, "any-", 4) == 0;
    /* Every node of the capture must pass, or with any- one of them. */
    bool any_passes = false;
    bool all_pass = true;
    for (uint16_t index = 0; index < call->match->capture_count; index += 1) {
        if (call->match->captures[index].index != call->steps[1].value_id) {
            continue;
        }
        bool passes = false;
        const sxml_status status = test_node(
            highlighter,
            &(node_test){.call = call, .predicate_name = name,
                         .node = call->match->captures[index].node},
            &passes);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        passes = passes != is_negated;
        any_passes = any_passes || passes;
        all_pass = all_pass && passes;
    }
    *holds = is_any ? any_passes : all_pass;
    return SXML_STATUS_OK;
}

/* Whether every predicate of the match's pattern holds. */
static sxml_status match_holds(highlighter *highlighter, const TSQueryMatch *match,
                               bool *holds) {
    uint32_t step_count = 0;
    const TSQueryPredicateStep *steps =
        ts_query_predicates_for_pattern(highlighter->query, match->pattern_index, &step_count);
    *holds = true;
    uint32_t first_step = 0;
    for (uint32_t step_index = 0; step_index < step_count && *holds; step_index += 1) {
        if (steps[step_index].type != TSQueryPredicateStepTypeDone) {
            continue;
        }
        const sxml_status status = evaluate_predicate(
            highlighter,
            &(predicate_call){.match = match,
                              .steps = steps + first_step,
                              .step_count = step_index - first_step},
            holds);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        first_step = step_index + 1;
    }
    return SXML_STATUS_OK;
}

static int compare_captures(const void *first, const void *second) {
    const highlight_capture *first_capture = first;
    const highlight_capture *second_capture = second;
    if (first_capture->node_identity != second_capture->node_identity) {
        return first_capture->node_identity < second_capture->node_identity ? -1 : 1;
    }
    /* The last pattern first: it is the one kept. */
    if (first_capture->pattern_index != second_capture->pattern_index) {
        return first_capture->pattern_index > second_capture->pattern_index ? -1 : 1;
    }
    return 0;
}

/* Sorts the captures and keeps the one of the last pattern of each node. */
static void keep_first_captures(sxml_xml_array *captures) {
    if (captures->element_count == 0) {
        return;
    }
    qsort(captures->elements, captures->element_count, sizeof(highlight_capture),
          compare_captures);
    highlight_capture *entries = sxml_xml_array_at(captures, 0);
    size_t kept_count = 1;
    for (size_t entry_index = 1; entry_index < captures->element_count; entry_index += 1) {
        if (entries[entry_index].node_identity != entries[kept_count - 1].node_identity) {
            entries[kept_count] = entries[entry_index];
            kept_count += 1;
        }
    }
    captures->element_count = kept_count;
}

static sxml_status record_match(highlighter *highlighter, const TSQueryMatch *match) {
    for (uint16_t index = 0; index < match->capture_count; index += 1) {
        const TSQueryCapture capture = match->captures[index];
        if (!highlighter->is_highlight_capture[capture.index]) {
            continue;
        }
        void *element = NULL;
        const sxml_status status = sxml_xml_array_push(&highlighter->captures, &element);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *(highlight_capture *)element =
            (highlight_capture){.node_identity = (uintptr_t)capture.node.id,
                                .pattern_index = match->pattern_index,
                                .capture_index = capture.index};
    }
    return SXML_STATUS_OK;
}

/* TSQueryCursorOptions.progress_callback: true cancels the query. */
static bool report_query_progress(TSQueryCursorState *state) {
    return !count_steps(state->payload, 1);
}

/* Runs the query over root and records the highlights. */
static sxml_status run_query(highlighter *highlighter, TSNode root) {
    TSQueryCursor *cursor = ts_query_cursor_new();
    if (cursor == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    ts_query_cursor_exec_with_options(
        cursor, highlighter->query, root,
        &(TSQueryCursorOptions){.payload = highlighter->account,
                                .progress_callback = report_query_progress});
    sxml_status status = SXML_STATUS_OK;
    TSQueryMatch match;
    while (status == SXML_STATUS_OK && ts_query_cursor_next_match(cursor, &match)) {
        bool holds = true;
        status = match_holds(highlighter, &match, &holds);
        if (status == SXML_STATUS_OK && holds) {
            status = record_match(highlighter, &match);
        }
        if (status == SXML_STATUS_OK && !count_steps(highlighter->account, 1)) {
            status = highlighter->account->status;
        }
    }
    ts_query_cursor_delete(cursor);
    if (status == SXML_STATUS_OK) {
        status = settle_steps(highlighter->account);
    }
    if (status == SXML_STATUS_OK) {
        keep_first_captures(&highlighter->captures);
    }
    return status;
}

/* Compiles the language's query and the tables run_query needs. */
static sxml_status prepare_highlighter(highlighter *highlighter, const TSLanguage *grammar) {
    const sxml_byte_view query_text = highlighter->request->language->highlights_query;
    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    highlighter->query = ts_query_new(grammar, (const char *)query_text.bytes,
                                      (uint32_t)query_text.byte_count, &error_offset, &error_type);
    if (highlighter->query == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator allocator = highlighter->request->allocator;
    const uint32_t capture_count = ts_query_capture_count(highlighter->query);
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = capture_count,
                                        .element_size = sizeof(bool),
                                        .element_alignment = alignof(bool)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    highlighter->is_highlight_capture = memory;
    for (uint32_t capture_index = 0; capture_index < capture_count; capture_index += 1) {
        uint32_t length = 0;
        const char *name =
            ts_query_capture_name_for_id(highlighter->query, capture_index, &length);
        highlighter->is_highlight_capture[capture_index] = length > 0 && name[0] != '_';
    }
    highlighter->regex_count = ts_query_string_count(highlighter->query);
    memory = NULL;
    status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = highlighter->regex_count,
                                        .element_size = sizeof(predicate_regex),
                                        .element_alignment = alignof(predicate_regex)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    highlighter->regexes = memory;
    for (uint32_t regex_index = 0; regex_index < highlighter->regex_count; regex_index += 1) {
        highlighter->regexes[regex_index] =
            (predicate_regex){.regex = NULL, .matcher = NULL, .is_tried = false};
    }
    return SXML_STATUS_OK;
}

static void finalize_highlighter(highlighter *highlighter) {
    const sxml_allocator allocator = highlighter->request->allocator;
    for (uint32_t regex_index = 0;
         highlighter->regexes != NULL && regex_index < highlighter->regex_count;
         regex_index += 1) {
        sxml_regex_matcher_destroy(highlighter->regexes[regex_index].matcher);
        sxml_regex_destroy(highlighter->regexes[regex_index].regex);
    }
    sxml_allocator_release(allocator, highlighter->regexes);
    sxml_allocator_release(allocator, highlighter->is_highlight_capture);
    sxml_xml_array_release(&highlighter->captures);
    if (highlighter->query != NULL) {
        ts_query_delete(highlighter->query);
    }
    highlighter->query = NULL;
}

/* The highlight of node: its capture's name, or the empty view. */
static sxml_byte_view find_highlight(const highlighter *highlighter, TSNode node) {
    const sxml_byte_view none = {.bytes = NULL, .byte_count = 0};
    if (highlighter == NULL || highlighter->captures.element_count == 0) {
        return none;
    }
    const highlight_capture *entries = sxml_xml_array_at(&highlighter->captures, 0);
    const uintptr_t identity = (uintptr_t)node.id;
    size_t low = 0;
    size_t high = highlighter->captures.element_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (entries[middle].node_identity < identity) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == highlighter->captures.element_count || entries[low].node_identity != identity) {
        return none;
    }
    uint32_t length = 0;
    const char *name =
        ts_query_capture_name_for_id(highlighter->query, entries[low].capture_index, &length);
    return (sxml_byte_view){.bytes = (const uint8_t *)name, .byte_count = length};
}

/* ---------------------------------------------------------- The document */

/* How a syntax node appears (src/source_code_internal.h). */
typedef enum node_shape {
    /* Text: an anonymous node with nothing to say. */
    SXML_SOURCE_CODE_NODE_SHAPE_TEXT,
    /* An element named by the node's kind. */
    SXML_SOURCE_CODE_NODE_SHAPE_ELEMENT,
    /* An sxml:node element: a named node whose kind is no NCName. */
    SXML_SOURCE_CODE_NODE_SHAPE_KIND_ELEMENT,
    /* An sxml:token element (glossary `token_element`). */
    SXML_SOURCE_CODE_NODE_SHAPE_TOKEN,
} node_shape;

/* Names interned once per document. */
enum {
    SXML_SOURCE_CODE_ATTRIBUTE_FIELD,
    SXML_SOURCE_CODE_ATTRIBUTE_HIGHLIGHT,
    SXML_SOURCE_CODE_ATTRIBUTE_MISSING,
    SXML_SOURCE_CODE_ATTRIBUTE_KIND,
    SXML_SOURCE_CODE_ATTRIBUTE_LINE,
    SXML_SOURCE_CODE_ATTRIBUTE_COLUMN,
    SXML_SOURCE_CODE_ATTRIBUTE_END_LINE,
    SXML_SOURCE_CODE_ATTRIBUTE_END_COLUMN,
    SXML_SOURCE_CODE_ATTRIBUTE_COUNT,
};

static const char *const attribute_local_names[SXML_SOURCE_CODE_ATTRIBUTE_COUNT] = {
    [SXML_SOURCE_CODE_ATTRIBUTE_FIELD] = "field",       [SXML_SOURCE_CODE_ATTRIBUTE_HIGHLIGHT] = "highlight",
    [SXML_SOURCE_CODE_ATTRIBUTE_MISSING] = "missing",   [SXML_SOURCE_CODE_ATTRIBUTE_KIND] = "kind",
    [SXML_SOURCE_CODE_ATTRIBUTE_LINE] = "line",         [SXML_SOURCE_CODE_ATTRIBUTE_COLUMN] = "column",
    [SXML_SOURCE_CODE_ATTRIBUTE_END_LINE] = "end-line", [SXML_SOURCE_CODE_ATTRIBUTE_END_COLUMN] = "end-column",
};

/* The state of cached element names, by symbol. */
enum {
    SXML_SOURCE_CODE_KIND_NAME_UNKNOWN = 0,
    SXML_SOURCE_CODE_KIND_NAME_INTERNED = 1,
    SXML_SOURCE_CODE_KIND_NAME_NOT_NCNAME = 2,
};

typedef struct document_writer {
    const sxml_source_code_request *request;
    step_account *account;
    const highlighter *highlighter;
    const TSLanguage *grammar;
    sxml_name_identifier attribute_names[SXML_SOURCE_CODE_ATTRIBUTE_COUNT];
    sxml_name_identifier token_name;
    sxml_name_identifier kind_element_name;
    sxml_namespace_prefix_identifier sxml_prefix;
    /* By symbol below symbol_count: the interned kind and its state. */
    sxml_name_identifier *kind_names;
    uint8_t *kind_name_states;
    uint32_t symbol_count;
    /* The source bytes written as text so far. */
    size_t written_byte_count;
    /* By open node below the document element: whether it made an
       element (bool). */
    sxml_xml_array open_nodes;
} document_writer;

static sxml_status intern_local_name(document_writer *writer, sxml_byte_view local_name,
                                     sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(
        writer->request->name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = local_name},
        name);
}

static sxml_status prepare_writer(document_writer *writer) {
    sxml_name_pool *name_pool = writer->request->name_pool;
    sxml_status status = SXML_STATUS_OK;
    for (size_t attribute_index = 0; status == SXML_STATUS_OK && attribute_index < SXML_SOURCE_CODE_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        status = intern_local_name(
            writer, sxml_byte_view_from_c_string(attribute_local_names[attribute_index]),
            &writer->attribute_names[attribute_index]);
    }
    const sxml_namespace_uri_identifier sxml_namespace =
        sxml_well_known_namespace_identifier(SXML_NAMESPACE_SXML);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = sxml_namespace,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("token")},
            &writer->token_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = sxml_namespace,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("node")},
            &writer->kind_element_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, SXML_BYTE_VIEW_FROM_LITERAL("sxml"), &writer->sxml_prefix);
    }
    writer->symbol_count = ts_language_symbol_count(writer->grammar);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            writer->request->allocator,
            (sxml_array_allocation_request){.element_count = writer->symbol_count,
                                            .element_size = sizeof(sxml_name_identifier),
                                            .element_alignment = alignof(sxml_name_identifier)},
            &memory);
    }
    writer->kind_names = memory;
    memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            writer->request->allocator,
            (sxml_array_allocation_request){.element_count = writer->symbol_count,
                                            .element_size = sizeof(uint8_t),
                                            .element_alignment = alignof(uint8_t)},
            &memory);
    }
    writer->kind_name_states = memory;
    if (status == SXML_STATUS_OK && writer->symbol_count > 0) {
        memset(writer->kind_name_states, SXML_SOURCE_CODE_KIND_NAME_UNKNOWN, writer->symbol_count);
    }
    return status;
}

static void finalize_writer(document_writer *writer) {
    sxml_allocator_release(writer->request->allocator, writer->kind_names);
    sxml_allocator_release(writer->request->allocator, writer->kind_name_states);
    sxml_xml_array_release(&writer->open_nodes);
}

/* The element name of a named node's kind; is_ncname false when the kind
   is no NCName (the node becomes an sxml:node element). */
typedef struct kind_name {
    sxml_name_identifier name;
    bool is_ncname;
} kind_name;

static sxml_status find_kind_name(document_writer *writer, TSNode node, kind_name *found) {
    const TSSymbol symbol = ts_node_symbol(node);
    const bool is_cached = symbol < writer->symbol_count;
    if (is_cached && writer->kind_name_states[symbol] != SXML_SOURCE_CODE_KIND_NAME_UNKNOWN) {
        found->is_ncname = writer->kind_name_states[symbol] == SXML_SOURCE_CODE_KIND_NAME_INTERNED;
        found->name = writer->kind_names[symbol];
        return SXML_STATUS_OK;
    }
    const sxml_byte_view kind = sxml_byte_view_from_c_string(ts_node_type(node));
    found->is_ncname = sxml_xml_text_is_ncname(kind);
    found->name = writer->kind_element_name;
    sxml_status status = SXML_STATUS_OK;
    if (found->is_ncname) {
        status = intern_local_name(writer, kind, &found->name);
    }
    if (status == SXML_STATUS_OK && is_cached) {
        writer->kind_name_states[symbol] =
            found->is_ncname ? SXML_SOURCE_CODE_KIND_NAME_INTERNED : SXML_SOURCE_CODE_KIND_NAME_NOT_NCNAME;
        writer->kind_names[symbol] = found->name;
    }
    return status;
}

static sxml_status write_text(document_writer *writer, size_t end_byte_offset) {
    const sxml_byte_view source = writer->request->source_text;
    if (end_byte_offset > source.byte_count) {
        end_byte_offset = source.byte_count;
    }
    if (end_byte_offset <= writer->written_byte_count) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view text = {.bytes = source.bytes + writer->written_byte_count,
                                 .byte_count = end_byte_offset - writer->written_byte_count};
    writer->written_byte_count = end_byte_offset;
    const sxml_tree_builder builder = writer->request->builder;
    return builder.operations->add_text(
        builder.state, &(sxml_text_event){.text = text,
                                          .location = {.line_number = 0},
                                          .is_cdata_section = false,
                                          .disables_output_escaping = false});
}

static sxml_status write_attribute(document_writer *writer, size_t attribute,
                                   sxml_byte_view value) {
    const sxml_tree_builder builder = writer->request->builder;
    return builder.operations->add_attribute(
        builder.state, &(sxml_attribute_event){.attribute_name = writer->attribute_names[attribute],
                                               .namespace_prefix = {.index = 0},
                                               .attribute_value = value,
                                               .type_annotation = {.index = SXML_TYPE_NONE},
                                               .is_id = false,
                                               .is_idrefs = false,
                                               .location = {.line_number = 0}});
}

/* A line or column number as decimal digits in digits. */
typedef struct decimal_text {
    char digits[24];
} decimal_text;

static sxml_byte_view format_number(uint64_t number, decimal_text *text) {
    size_t start = sizeof(text->digits);
    do {
        start -= 1;
        text->digits[start] = (char)('0' + (number % 10U));
        number /= 10U;
    } while (number > 0 && start > 0);
    return (sxml_byte_view){.bytes = (const uint8_t *)text->digits + start,
                            .byte_count = sizeof(text->digits) - start};
}

/* A place in the source: its line and column from 1, the column in
   characters (codepoints) from the start of the line. */
typedef struct source_place {
    uint64_t line;
    uint64_t column;
} source_place;

static source_place place_of(const document_writer *writer, TSPoint point, uint32_t byte_offset) {
    const sxml_byte_view source = writer->request->source_text;
    size_t end = byte_offset <= source.byte_count ? byte_offset : source.byte_count;
    size_t line_start = point.column <= end ? end - point.column : 0;
    uint64_t column = 1;
    for (size_t offset = line_start; offset < end; offset += 1) {
        if ((source.bytes[offset] & 0xC0U) != 0x80U) {
            column += 1;
        }
    }
    return (source_place){.line = (uint64_t)point.row + 1U, .column = column};
}

static sxml_status write_place(document_writer *writer, source_place place, size_t line_attribute) {
    decimal_text text;
    sxml_status status = write_attribute(writer, line_attribute, format_number(place.line, &text));
    if (status == SXML_STATUS_OK) {
        status = write_attribute(writer, line_attribute + 1, format_number(place.column, &text));
    }
    return status;
}

/* The end of the whole text, for the document element. */
static source_place text_end_place(const document_writer *writer) {
    const sxml_byte_view source = writer->request->source_text;
    uint64_t line = 1;
    size_t line_start = 0;
    for (size_t offset = 0; offset < source.byte_count; offset += 1) {
        if (source.bytes[offset] == '\n') {
            line += 1;
            line_start = offset + 1;
        }
    }
    const TSPoint point = {.row = (uint32_t)(line - 1),
                           .column = (uint32_t)(source.byte_count - line_start)};
    return place_of(writer, point, (uint32_t)source.byte_count);
}

/* What a node is and says: its shape, field and highlight. */
typedef struct node_visit {
    TSNode node;
    sxml_byte_view field_name;
    bool is_document_element;
} node_visit;

static node_shape shape_of(const node_visit *visit, sxml_byte_view highlight) {
    if (ts_node_is_named(visit->node)) {
        return SXML_SOURCE_CODE_NODE_SHAPE_ELEMENT;
    }
    if (ts_node_is_missing(visit->node) || highlight.byte_count > 0 ||
        visit->field_name.byte_count > 0) {
        return SXML_SOURCE_CODE_NODE_SHAPE_TOKEN;
    }
    return SXML_SOURCE_CODE_NODE_SHAPE_TEXT;
}

/* Starts the element of a node that makes one, with its attributes;
   *makes_element false for a node that is text. */
static sxml_status start_node(document_writer *writer, const node_visit *visit,
                              bool *makes_element) {
    const TSNode node = visit->node;
    const sxml_byte_view highlight = find_highlight(writer->highlighter, node);
    node_shape shape = shape_of(visit, highlight);
    *makes_element = shape != SXML_SOURCE_CODE_NODE_SHAPE_TEXT;
    if (!*makes_element) {
        return SXML_STATUS_OK;
    }
    sxml_name_identifier element_name = writer->token_name;
    sxml_namespace_prefix_identifier prefix = writer->sxml_prefix;
    sxml_status status = SXML_STATUS_OK;
    if (shape == SXML_SOURCE_CODE_NODE_SHAPE_ELEMENT) {
        kind_name found = {.name = {.index = 0}, .is_ncname = true};
        status = find_kind_name(writer, node, &found);
        element_name = found.name;
        if (found.is_ncname) {
            prefix = (sxml_namespace_prefix_identifier){.index = 0};
        } else {
            shape = SXML_SOURCE_CODE_NODE_SHAPE_KIND_ELEMENT;
        }
    }
    const sxml_tree_builder builder = writer->request->builder;
    if (status == SXML_STATUS_OK) {
        status = builder.operations->start_element(
            builder.state, &(sxml_element_start_event){.element_name = element_name,
                                                       .namespace_prefix = prefix,
                                                       .type_annotation = {.index = SXML_TYPE_NONE},
                                                       .is_id = false,
                                                       .is_idrefs = false,
                                                       .is_nilled = false,
                                                       .ignores_parent_namespaces = false,
                                                       .location = {.line_number = 0},
                                                       .entity_base_uri = {.bytes = NULL,
                                                                           .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && visit->is_document_element) {
        status = builder.operations->add_namespace_binding(
            builder.state,
            &(sxml_namespace_binding){
                .namespace_prefix = writer->sxml_prefix,
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_SXML)});
    }
    if (status == SXML_STATUS_OK && visit->field_name.byte_count > 0) {
        status = write_attribute(writer, SXML_SOURCE_CODE_ATTRIBUTE_FIELD, visit->field_name);
    }
    if (status == SXML_STATUS_OK && highlight.byte_count > 0) {
        status = write_attribute(writer, SXML_SOURCE_CODE_ATTRIBUTE_HIGHLIGHT, highlight);
    }
    const bool is_missing = ts_node_is_missing(node);
    if (status == SXML_STATUS_OK && is_missing) {
        status = write_attribute(writer, SXML_SOURCE_CODE_ATTRIBUTE_MISSING, SXML_BYTE_VIEW_FROM_LITERAL("true"));
    }
    if (status == SXML_STATUS_OK &&
        (shape == SXML_SOURCE_CODE_NODE_SHAPE_KIND_ELEMENT || (shape == SXML_SOURCE_CODE_NODE_SHAPE_TOKEN && is_missing))) {
        status =
            write_attribute(writer, SXML_SOURCE_CODE_ATTRIBUTE_KIND, sxml_byte_view_from_c_string(ts_node_type(node)));
    }
    if (status == SXML_STATUS_OK && writer->request->has_positions) {
        const source_place start =
            visit->is_document_element
                ? (source_place){.line = 1, .column = 1}
                : place_of(writer, ts_node_start_point(node), ts_node_start_byte(node));
        const source_place end =
            visit->is_document_element
                ? text_end_place(writer)
                : place_of(writer, ts_node_end_point(node), ts_node_end_byte(node));
        status = write_place(writer, start, SXML_SOURCE_CODE_ATTRIBUTE_LINE);
        if (status == SXML_STATUS_OK) {
            status = write_place(writer, end, SXML_SOURCE_CODE_ATTRIBUTE_END_LINE);
        }
    }
    return status;
}

static sxml_status end_node(document_writer *writer, TSNode node, bool makes_element) {
    sxml_status status = write_text(writer, ts_node_end_byte(node));
    if (status == SXML_STATUS_OK && makes_element) {
        const sxml_tree_builder builder = writer->request->builder;
        status = builder.operations->end_element(builder.state);
    }
    return status;
}

static sxml_status push_open_node(document_writer *writer, bool makes_element) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&writer->open_nodes, &element);
    if (status == SXML_STATUS_OK) {
        *(bool *)element = makes_element;
    }
    return status;
}

static bool pop_open_node(document_writer *writer) {
    writer->open_nodes.element_count -= 1;
    return *(const bool *)sxml_xml_array_at(&writer->open_nodes, writer->open_nodes.element_count);
}

/* Writes the descendants of the cursor's node (the root), depth first in
   one loop; the cursor ends on the root again. */
static sxml_status write_descendants(document_writer *writer, TSTreeCursor *cursor) {
    if (!ts_tree_cursor_goto_first_child(cursor)) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        const TSNode node = ts_tree_cursor_current_node(cursor);
        const char *field_name = ts_tree_cursor_current_field_name(cursor);
        bool makes_element = false;
        status = write_text(writer, ts_node_start_byte(node));
        if (status == SXML_STATUS_OK) {
            status = start_node(
                writer,
                &(node_visit){.node = node,
                              .field_name = field_name == NULL
                                                ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                                : sxml_byte_view_from_c_string(field_name),
                              .is_document_element = false},
                &makes_element);
        }
        if (status == SXML_STATUS_OK && !count_steps(writer->account, 1)) {
            status = writer->account->status;
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (ts_tree_cursor_goto_first_child(cursor)) {
            status = push_open_node(writer, makes_element);
            continue;
        }
        status = end_node(writer, node, makes_element);
        /* Up to the next sibling, ending the nodes left behind. */
        while (status == SXML_STATUS_OK && !ts_tree_cursor_goto_next_sibling(cursor)) {
            if (writer->open_nodes.element_count == 0) {
                (void)ts_tree_cursor_goto_parent(cursor);
                return SXML_STATUS_OK;
            }
            (void)ts_tree_cursor_goto_parent(cursor);
            const bool parent_makes_element = pop_open_node(writer);
            status = end_node(writer, ts_tree_cursor_current_node(cursor), parent_makes_element);
        }
    }
    return status;
}

static sxml_status write_document(document_writer *writer, TSNode root) {
    const sxml_tree_builder builder = writer->request->builder;
    sxml_status status = builder.operations->start_document(
        builder.state,
        &(sxml_document_start_event){.document_uri = {.bytes = NULL, .byte_count = 0},
                                     .base_uri = {.bytes = NULL, .byte_count = 0},
                                     .location = {.line_number = 0}});
    bool makes_element = false;
    if (status == SXML_STATUS_OK) {
        status = start_node(writer,
                            &(node_visit){.node = root,
                                          .field_name = {.bytes = NULL, .byte_count = 0},
                                          .is_document_element = true},
                            &makes_element);
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    if (status == SXML_STATUS_OK) {
        status = write_descendants(writer, &cursor);
    }
    ts_tree_cursor_delete(&cursor);
    /* The document element holds the whole text. */
    if (status == SXML_STATUS_OK) {
        status = write_text(writer, writer->request->source_text.byte_count);
    }
    if (status == SXML_STATUS_OK && makes_element) {
        status = builder.operations->end_element(builder.state);
    }
    if (status == SXML_STATUS_OK) {
        status = settle_steps(writer->account);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    return status;
}

/* ----------------------------------------------------------------- Parse */

sxml_status sxml_source_code_parse(const sxml_source_code_request *request) {
    if (request == NULL || request->language == NULL || request->builder.operations == NULL ||
        request->name_pool == NULL ||
        (request->source_text.bytes == NULL && request->source_text.byte_count > 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->source_text.byte_count >= UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    step_account account = {
        .meter = request->step_meter, .pending_step_count = 0, .status = SXML_STATUS_OK};
    TSTree *tree = NULL;
    sxml_status status = parse_tree(request, &account, &tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const TSNode root = ts_tree_root_node(tree);
    const TSLanguage *grammar = ts_tree_language(tree);
    highlighter highlights = {
        .request = request,
        .account = &account,
        .query = NULL,
        .captures = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = request->allocator, .element_byte_count = sizeof(highlight_capture)}),
        .regexes = NULL,
        .regex_count = 0,
        .is_highlight_capture = NULL};
    if (request->has_highlights) {
        status = prepare_highlighter(&highlights, grammar);
        if (status == SXML_STATUS_OK) {
            status = run_query(&highlights, root);
        }
    }
    document_writer writer = {
        .request = request,
        .account = &account,
        .highlighter = request->has_highlights ? &highlights : NULL,
        .grammar = grammar,
        .attribute_names = {{.index = 0}},
        .token_name = {.index = 0},
        .kind_element_name = {.index = 0},
        .sxml_prefix = {.index = 0},
        .kind_names = NULL,
        .kind_name_states = NULL,
        .symbol_count = 0,
        .written_byte_count = 0,
        .open_nodes = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = request->allocator, .element_byte_count = sizeof(bool)})};
    if (status == SXML_STATUS_OK) {
        status = prepare_writer(&writer);
    }
    if (status == SXML_STATUS_OK) {
        status = write_document(&writer, root);
    }
    finalize_writer(&writer);
    finalize_highlighter(&highlights);
    ts_tree_delete(tree);
    return status;
}
