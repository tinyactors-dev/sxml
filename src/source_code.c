/*
 * Source code as XML (src/source_code_internal.h; packets 7.22 and 7.23,
 * D285, D288): a tree-sitter syntax tree as tree builder events, with the
 * captures of the grammar's highlights query as attributes, and the code
 * inside code its injections query names parsed with its own language.
 *
 * Four steps for each tree, each bounded by the request's step meter:
 * 1. tree-sitter parses the text (D12); its progress callback charges the
 *    bytes read and cancels the parse when the meter refuses.
 * 2. With highlights, the locals query (glossary `locals_query`) runs
 *    first: its captures, in document order, open scopes, define names
 *    and refer to them; a reference that finds a definition of the same
 *    text in its scope or an enclosing one that inherits is a local
 *    reference (tree-sitter's highlight crate, "local variables"). Then
 *    the highlights query runs. Each match whose text predicates hold
 *    (eq?, match?, any-of? and their negations, and "is? local" for a
 *    match with a local reference; tree-sitter leaves predicates to the
 *    host) records its captures; for a node that several patterns
 *    capture, the last pattern in the query wins, as the grammars'
 *    highlight tests expect (D285). A local reference then takes the
 *    highlight of its definition. Captures whose names start with "_"
 *    only serve predicates. The records are sorted by node, so the walk
 *    finds a node's highlight by binary search.
 * 3. With injections, the injections query (glossary `injections_query`)
 *    names nodes whose text is in another language: injection.content
 *    with injection.language (a capture's text, or "#set!"), or
 *    injection.self; "injection.include-children" keeps the node's
 *    children in the text. Each node is parsed on its own
 *    (injection.combined is not supported: each node is its own
 *    document).
 * 4. One loop walks the trees with TSTreeCursors, depth first. An
 *    explicit stack of layers (D71) holds the document's tree and, above
 *    it, the trees of the injections being written: when the walk meets
 *    an injection's node, it starts the node's element, parses the
 *    node's text (the node's range without its children's, through
 *    tree-sitter's included ranges) with the injected language and
 *    pushes its tree; when that tree is written, the layer is popped and
 *    the node's element ended. A second explicit stack keeps which open
 *    nodes made elements. Text is written by byte offset: whatever lies
 *    between the end of what was written and the start of the next node
 *    is written first, so no byte of the source is lost or written twice.
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

/* Most injections inside one another (layers above the document's). */
#define SXML_SOURCE_CODE_MAXIMUM_INJECTION_DEPTH 8U

/* ------------------------------------------------------------ Languages */

static bool byte_views_equal(sxml_byte_view first, sxml_byte_view second) {
    return first.byte_count == second.byte_count &&
           (first.byte_count == 0 || memcmp(first.bytes, second.bytes, first.byte_count) == 0);
}

sxml_source_code_language sxml_source_code_builtin_language_at(size_t language_index) {
    const sxml_source_code_builtin_language *builtin =
        &sxml_source_code_builtin_languages.languages[language_index];
    return (sxml_source_code_language){.language_name = builtin->language_name,
                                       .grammar = builtin->get_grammar(),
                                       .highlights_query = builtin->highlights_query,
                                       .locals_query = builtin->locals_query,
                                       .injections_query = builtin->injections_query,
                                       .injection_regex = builtin->injection_regex};
}

bool sxml_source_code_find_language(const sxml_source_code_language_set *set,
                                    sxml_byte_view name, sxml_source_code_language *language) {
    const sxml_source_code_language_view registered = set->registered_languages;
    for (size_t language_index = 0; language_index < registered.language_count;
         language_index += 1) {
        if (byte_views_equal(registered.languages[language_index].language_name, name)) {
            *language = registered.languages[language_index];
            return true;
        }
    }
    for (size_t language_index = 0;
         language_index < sxml_source_code_builtin_languages.language_count;
         language_index += 1) {
        if (byte_views_equal(
                sxml_source_code_builtin_languages.languages[language_index].language_name, name)) {
            *language = sxml_source_code_builtin_language_at(language_index);
            return true;
        }
    }
    return false;
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

/* What one parse reads: the grammar, the whole text, and the parts of
   it the tree covers (all of it when range_count is 0). */
typedef struct parse_job {
    const TSLanguage *grammar;
    sxml_byte_view source_text;
    const TSRange *ranges;
    uint32_t range_count;
} parse_job;

/* Parses job's text into *tree (NULL on failure). */
static sxml_status parse_tree(const parse_job *job, step_account *account, TSTree **tree) {
    *tree = NULL;
    TSParser *parser = ts_parser_new();
    if (parser == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    if (!ts_parser_set_language(parser, job->grammar) ||
        (job->range_count > 0 &&
         !ts_parser_set_included_ranges(parser, job->ranges, job->range_count))) {
        ts_parser_delete(parser);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    source_input input = {.text = job->source_text};
    parse_progress progress = {
        .account = account,
        .last_byte_offset = job->range_count > 0 ? job->ranges[0].start_byte : 0};
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

/* ------------------------------------------------------------- Queries */

/* A compiled regex of a match? predicate, by its string's identifier. */
typedef struct predicate_regex {
    sxml_regex *regex;
    sxml_regex_matcher *matcher;
    /* Compiled, or found not to compile (then the predicate fails). */
    bool is_tried;
} predicate_regex;

/* One compiled query of a tree's language and the regexes of its
   predicates; query is NULL for a language without the query. */
typedef struct compiled_query {
    TSQuery *query;
    predicate_regex *regexes;
    uint32_t regex_count;
} compiled_query;

/* One highlight capture: the node (TSNode.id, unique in its tree), the
   pattern and the capture name. */
typedef struct highlight_capture {
    uintptr_t node_identity;
    uint32_t pattern_index;
    uint32_t capture_index;
} highlight_capture;

/* A local reference and the definition it found. */
typedef struct local_reference {
    uintptr_t reference_identity;
    uintptr_t definition_identity;
} local_reference;

/* A node whose text is in another language. */
typedef struct injection_site {
    uintptr_t node_identity;
    /* The language's name: the capture's text or a query string. */
    sxml_byte_view language_name;
    bool includes_children;
} injection_site;

/* The compiled queries of one language, kept for the whole document, so
   that a language injected many times (Markdown's inline grammar, once
   for every paragraph) compiles its queries once. A query that is not
   wanted, or that does not compile for an injected language, is NULL. */
typedef struct language_queries {
    const TSLanguage *grammar;
    /* The highlights query's text: with the grammar, which language this is. */
    const uint8_t *highlights_query_bytes;
    compiled_query highlights;
    compiled_query locals;
    compiled_query injections;
    /* By highlights capture index: true for a name that is a highlight. */
    bool *is_highlight_capture;
} language_queries;

/* What the queries of one tree's language say about it. The compiled
   queries are borrowed from the document's language_queries. */
typedef struct tree_analysis {
    const sxml_source_code_request *request;
    step_account *account;
    sxml_source_code_language language;
    compiled_query highlights;
    compiled_query locals;
    compiled_query injections;
    /* By highlights capture index: true for a name that is a highlight. */
    const bool *is_highlight_capture;
    /* highlight_capture, sorted by node, one per node. */
    sxml_xml_array captures;
    /* local_reference, sorted by reference. */
    sxml_xml_array local_references;
    /* injection_site, sorted by node, one per node. */
    sxml_xml_array injection_sites;
} tree_analysis;

static sxml_byte_view node_text(const tree_analysis *analysis, TSNode node) {
    const sxml_byte_view text = analysis->request->source_text;
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

static sxml_byte_view query_string(const compiled_query *compiled, uint32_t value_id) {
    uint32_t length = 0;
    const char *value = ts_query_string_value_for_id(compiled->query, value_id, &length);
    return (sxml_byte_view){.bytes = (const uint8_t *)value, .byte_count = length};
}

static sxml_byte_view capture_name(const compiled_query *compiled, uint32_t capture_index) {
    uint32_t length = 0;
    const char *name = ts_query_capture_name_for_id(compiled->query, capture_index, &length);
    return (sxml_byte_view){.bytes = (const uint8_t *)name, .byte_count = length};
}

/* A regex and its matcher, compiled from text; both NULL when the text
   is no XPath regex. */
typedef struct regex_pair {
    sxml_regex *regex;
    sxml_regex_matcher *matcher;
} regex_pair;

static sxml_status compile_regex(sxml_allocator allocator, sxml_byte_view text, regex_pair *pair) {
    *pair = (regex_pair){.regex = NULL, .matcher = NULL};
    sxml_string regex_text;
    sxml_status status = sxml_string_from_utf8(text, &regex_text);
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_create(
            allocator,
            &(sxml_regex_configuration){.regex_text = regex_text,
                                        .flags = {.is_dot_all = false},
                                        .maximum_instruction_count = 0,
                                        .maximum_group_depth = 0,
                                        .stack_allocator = {.state = NULL, .operations = NULL},
                                        .error_record = NULL},
            &pair->regex);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_create(
            allocator,
            &(sxml_regex_matcher_configuration){.regex = pair->regex,
                                                .backtrack_allocator = allocator,
                                                .initial_backtrack_entry_count = 0,
                                                .maximum_backtrack_byte_count = 0,
                                                .maximum_step_count = 0},
            &pair->matcher);
    }
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_INVALID_UTF8) {
        status = SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK || pair->matcher == NULL) {
        sxml_regex_matcher_destroy(pair->matcher);
        sxml_regex_destroy(pair->regex);
        *pair = (regex_pair){.regex = NULL, .matcher = NULL};
    }
    return status;
}

static void release_regex(regex_pair *pair) {
    sxml_regex_matcher_destroy(pair->matcher);
    sxml_regex_destroy(pair->regex);
    *pair = (regex_pair){.regex = NULL, .matcher = NULL};
}

/* Whether the regex matches somewhere in text (Rust's is_match); the
   matcher's steps are in *step_count. */
typedef struct regex_search {
    sxml_regex_matcher *matcher;
    sxml_byte_view text;
} regex_search;

static sxml_status regex_matches(regex_search search, uint64_t *step_count, bool *matches) {
    sxml_string input;
    sxml_status status = sxml_string_from_utf8(search.text, &input);
    sxml_regex_search_result found = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_find(
            search.matcher,
            &(sxml_regex_search_request){.regex_input = input, .start_byte_offset = 0}, &found);
    }
    *step_count = sxml_regex_matcher_get_step_count(search.matcher);
    *matches = status == SXML_STATUS_OK && found.is_found;
    return status;
}

/* The regex of string value_id of a query, compiled at its first use;
   NULL when it does not compile as an XPath regex (D285: tree-sitter
   queries write Rust regexes, and the common subset means the same). */
typedef struct regex_lookup {
    compiled_query *compiled;
    uint32_t value_id;
} regex_lookup;

static sxml_status find_regex(const tree_analysis *analysis, regex_lookup lookup,
                              predicate_regex **found) {
    *found = NULL;
    compiled_query *compiled = lookup.compiled;
    if (lookup.value_id >= compiled->regex_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    predicate_regex *entry = &compiled->regexes[lookup.value_id];
    if (!entry->is_tried) {
        entry->is_tried = true;
        regex_pair pair = {.regex = NULL, .matcher = NULL};
        const sxml_status status = compile_regex(analysis->request->allocator,
                                                 query_string(compiled, lookup.value_id), &pair);
        entry->regex = pair.regex;
        entry->matcher = pair.matcher;
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (entry->matcher != NULL) {
        *found = entry;
    }
    return SXML_STATUS_OK;
}

/* One predicate of a match: its query, its steps (name first), without
   the DONE. */
typedef struct predicate_call {
    compiled_query *compiled;
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

static sxml_status test_node(const tree_analysis *analysis, const node_test *test, bool *passes) {
    const predicate_call *call = test->call;
    const sxml_byte_view text = node_text(analysis, test->node);
    *passes = true;
    if (has_base(test->predicate_name, "eq?")) {
        const TSQueryPredicateStep argument = call->steps[2];
        sxml_byte_view other = {.bytes = NULL, .byte_count = 0};
        if (argument.type == TSQueryPredicateStepTypeCapture) {
            TSNode other_node;
            if (!find_capture_node(call->match, argument.value_id, &other_node)) {
                return SXML_STATUS_OK;
            }
            other = node_text(analysis, other_node);
        } else {
            other = query_string(call->compiled, argument.value_id);
        }
        *passes = byte_views_equal(text, other);
        return SXML_STATUS_OK;
    }
    if (has_base(test->predicate_name, "match?") || has_base(test->predicate_name, "lua-match?")) {
        predicate_regex *entry = NULL;
        sxml_status status = find_regex(
            analysis, (regex_lookup){.compiled = call->compiled, .value_id = call->steps[2].value_id},
            &entry);
        if (status != SXML_STATUS_OK || entry == NULL) {
            *passes = false;
            return status;
        }
        uint64_t step_count = 0;
        status = regex_matches((regex_search){.matcher = entry->matcher, .text = text}, &step_count,
                               passes);
        if (status == SXML_STATUS_OK && !count_steps(analysis->account, step_count)) {
            status = analysis->account->status;
        }
        return status;
    }
    /* any-of?: one of the strings. */
    *passes = false;
    for (uint32_t step_index = 2; step_index < call->step_count; step_index += 1) {
        if (byte_views_equal(text, query_string(call->compiled, call->steps[step_index].value_id))) {
            *passes = true;
        }
    }
    return SXML_STATUS_OK;
}

static bool find_local_definition(const tree_analysis *analysis, uintptr_t identity,
                                  uintptr_t *definition_identity);

/* Whether a capture of the match is a local reference. */
static bool match_has_local_reference(const tree_analysis *analysis, const TSQueryMatch *match) {
    for (uint16_t index = 0; index < match->capture_count; index += 1) {
        uintptr_t definition_identity = 0;
        if (find_local_definition(analysis, (uintptr_t)match->captures[index].node.id,
                                  &definition_identity)) {
            return true;
        }
    }
    return false;
}

/* Evaluates one predicate; unknown predicates and directives (#set!)
   hold. */
static sxml_status evaluate_predicate(const tree_analysis *analysis, const predicate_call *call,
                                      bool *holds) {
    *holds = true;
    if (call->step_count == 0 || call->steps[0].type != TSQueryPredicateStepTypeString) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view name = query_string(call->compiled, call->steps[0].value_id);
    /* #is? local / #is-not? local: whether the match captures a local
       reference (the locals query, D288). */
    if (byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("is?")) ||
        byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("is-not?"))) {
        if (call->step_count >= 2 && call->steps[1].type == TSQueryPredicateStepTypeString &&
            byte_views_equal(query_string(call->compiled, call->steps[1].value_id),
                             SXML_BYTE_VIEW_FROM_LITERAL("local"))) {
            const bool is_local = match_has_local_reference(analysis, call->match);
            *holds = is_local == (name.byte_count == 3);
        }
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
            analysis,
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

/* A match of a query. */
typedef struct query_match {
    compiled_query *compiled;
    const TSQueryMatch *match;
} query_match;

/* Whether every predicate of the match's pattern holds. */
static sxml_status match_holds(const tree_analysis *analysis, query_match found, bool *holds) {
    uint32_t step_count = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(
        found.compiled->query, found.match->pattern_index, &step_count);
    *holds = true;
    uint32_t first_step = 0;
    for (uint32_t step_index = 0; step_index < step_count && *holds; step_index += 1) {
        if (steps[step_index].type != TSQueryPredicateStepTypeDone) {
            continue;
        }
        const sxml_status status = evaluate_predicate(
            analysis,
            &(predicate_call){.compiled = found.compiled,
                              .match = found.match,
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

/* A property of a pattern ("#set! KEY" or "#set! KEY VALUE"). */
typedef struct pattern_property {
    const compiled_query *compiled;
    uint32_t pattern_index;
    const char *key;
} pattern_property;

/* Whether the pattern sets the property; its value (empty without one) in
   *value. */
static bool find_property(pattern_property property, sxml_byte_view *value) {
    uint32_t step_count = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(
        property.compiled->query, property.pattern_index, &step_count);
    const sxml_byte_view key = sxml_byte_view_from_c_string(property.key);
    uint32_t first_step = 0;
    for (uint32_t step_index = 0; step_index < step_count; step_index += 1) {
        if (steps[step_index].type != TSQueryPredicateStepTypeDone) {
            continue;
        }
        const TSQueryPredicateStep *predicate = steps + first_step;
        const uint32_t length = step_index - first_step;
        first_step = step_index + 1;
        if (length >= 2 && predicate[0].type == TSQueryPredicateStepTypeString &&
            predicate[1].type == TSQueryPredicateStepTypeString &&
            byte_views_equal(query_string(property.compiled, predicate[0].value_id),
                             SXML_BYTE_VIEW_FROM_LITERAL("set!")) &&
            byte_views_equal(query_string(property.compiled, predicate[1].value_id), key)) {
            *value = length >= 3 && predicate[2].type == TSQueryPredicateStepTypeString
                         ? query_string(property.compiled, predicate[2].value_id)
                         : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
            return true;
        }
    }
    return false;
}

/* Compiles one query of the language; a query that does not compile
   leaves compiled->query NULL and gives SXML_STATUS_MALFORMED_INPUT. */
static sxml_status compile_query(const tree_analysis *analysis, sxml_byte_view query_text,
                                 compiled_query *compiled) {
    if (query_text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    if (query_text.byte_count >= UINT32_MAX) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    compiled->query = ts_query_new(analysis->language.grammar, (const char *)query_text.bytes,
                                   (uint32_t)query_text.byte_count, &error_offset, &error_type);
    if (compiled->query == NULL) {
        return error_type == TSQueryErrorNone ? SXML_STATUS_OUT_OF_MEMORY
                                              : SXML_STATUS_MALFORMED_INPUT;
    }
    compiled->regex_count = ts_query_string_count(compiled->query);
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        analysis->request->allocator,
        (sxml_array_allocation_request){.element_count = compiled->regex_count,
                                        .element_size = sizeof(predicate_regex),
                                        .element_alignment = alignof(predicate_regex)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    compiled->regexes = memory;
    for (uint32_t regex_index = 0; regex_index < compiled->regex_count; regex_index += 1) {
        compiled->regexes[regex_index] =
            (predicate_regex){.regex = NULL, .matcher = NULL, .is_tried = false};
    }
    return SXML_STATUS_OK;
}

static void finalize_query(sxml_allocator allocator, compiled_query *compiled) {
    for (uint32_t regex_index = 0; compiled->regexes != NULL && regex_index < compiled->regex_count;
         regex_index += 1) {
        sxml_regex_matcher_destroy(compiled->regexes[regex_index].matcher);
        sxml_regex_destroy(compiled->regexes[regex_index].regex);
    }
    sxml_allocator_release(allocator, compiled->regexes);
    if (compiled->query != NULL) {
        ts_query_delete(compiled->query);
    }
    *compiled = (compiled_query){.query = NULL, .regexes = NULL, .regex_count = 0};
}

/* TSQueryCursorOptions.progress_callback: true cancels the query. */
static bool report_query_progress(TSQueryCursorState *state) {
    return !count_steps(state->payload, 1);
}

/* A query to run: its options live in the caller's frame, as the cursor
   keeps a pointer to them until it is deleted. */
typedef struct query_start {
    const compiled_query *compiled;
    TSQueryCursorOptions *options;
} query_start;

static TSQueryCursor *start_query(const tree_analysis *analysis, query_start started,
                                  TSNode root) {
    *started.options = (TSQueryCursorOptions){.payload = analysis->account,
                                              .progress_callback = report_query_progress};
    TSQueryCursor *cursor = ts_query_cursor_new();
    if (cursor != NULL) {
        ts_query_cursor_exec_with_options(cursor, started.compiled->query, root, started.options);
    }
    return cursor;
}

/* Pushes one element of size element_size from value onto array. */
static sxml_status push_value(sxml_xml_array *array, const void *value, size_t element_size) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(array, &element);
    if (status == SXML_STATUS_OK) {
        memcpy(element, value, element_size);
    }
    return status;
}

/* --------------------------------------------------------------- Locals */

/* What a locals capture name means (tree-sitter's highlight crate). */
typedef enum local_role {
    SXML_SOURCE_CODE_LOCAL_ROLE_NONE,
    SXML_SOURCE_CODE_LOCAL_ROLE_SCOPE,
    SXML_SOURCE_CODE_LOCAL_ROLE_DEFINITION,
    SXML_SOURCE_CODE_LOCAL_ROLE_REFERENCE,
} local_role;

static local_role role_of(sxml_byte_view name) {
    if (byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("local.scope"))) {
        return SXML_SOURCE_CODE_LOCAL_ROLE_SCOPE;
    }
    if (byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("local.reference"))) {
        return SXML_SOURCE_CODE_LOCAL_ROLE_REFERENCE;
    }
    const sxml_byte_view definition = SXML_BYTE_VIEW_FROM_LITERAL("local.definition");
    if (name.byte_count >= definition.byte_count &&
        memcmp(name.bytes, definition.bytes, definition.byte_count) == 0 &&
        (name.byte_count == definition.byte_count || name.bytes[definition.byte_count] == '.')) {
        return SXML_SOURCE_CODE_LOCAL_ROLE_DEFINITION;
    }
    return SXML_SOURCE_CODE_LOCAL_ROLE_NONE;
}

/* An open scope: where it ends, whether names of enclosing scopes are
   seen in it, and its first definition. */
typedef struct local_scope {
    uint32_t end_byte;
    bool inherits;
    size_t first_definition;
} local_scope;

/* A definition: its node and the byte range of its name. */
typedef struct local_definition {
    uintptr_t node_identity;
    uint32_t start_byte;
    uint32_t end_byte;
} local_definition;

/* The scopes and definitions of the locals walk. */
typedef struct locals_walk {
    tree_analysis *analysis;
    sxml_xml_array scopes;
    sxml_xml_array definitions;
} locals_walk;

/* Looks the reference up from the innermost scope outwards. */
static sxml_status resolve_reference(locals_walk *walk, TSNode node) {
    const uintptr_t identity = (uintptr_t)node.id;
    const size_t definition_count = walk->definitions.element_count;
    if (definition_count > 0) {
        const local_definition *last = sxml_xml_array_at(&walk->definitions, definition_count - 1);
        if (last->node_identity == identity) {
            return SXML_STATUS_OK;
        }
    }
    const sxml_byte_view name = node_text(walk->analysis, node);
    size_t end = definition_count;
    for (size_t scope_index = walk->scopes.element_count; scope_index > 0; scope_index -= 1) {
        const local_scope *scope = sxml_xml_array_at(&walk->scopes, scope_index - 1);
        for (size_t definition_index = end; definition_index > scope->first_definition;
             definition_index -= 1) {
            const local_definition *definition =
                sxml_xml_array_at(&walk->definitions, definition_index - 1);
            const sxml_byte_view source = walk->analysis->request->source_text;
            const sxml_byte_view text = {.bytes = source.bytes + definition->start_byte,
                                         .byte_count = definition->end_byte - definition->start_byte};
            if (byte_views_equal(text, name)) {
                const local_reference reference = {.reference_identity = identity,
                                                   .definition_identity = definition->node_identity};
                return push_value(&walk->analysis->local_references, &reference, sizeof(reference));
            }
        }
        end = scope->first_definition;
        if (!scope->inherits) {
            break;
        }
    }
    return SXML_STATUS_OK;
}

/* Handles one locals capture, in document order. */
typedef struct locals_capture {
    query_match found;
    TSQueryCapture capture;
} locals_capture;

static sxml_status take_local_capture(locals_walk *walk, const locals_capture *taken) {
    tree_analysis *analysis = walk->analysis;
    const TSNode node = taken->capture.node;
    const uint32_t start = ts_node_start_byte(node);
    /* Leave the scopes that end before the node; the outermost one, the
       whole tree's, never ends. */
    while (walk->scopes.element_count > 1) {
        const local_scope *scope = sxml_xml_array_at(&walk->scopes, walk->scopes.element_count - 1);
        if (scope->end_byte > start) {
            break;
        }
        walk->definitions.element_count = scope->first_definition;
        walk->scopes.element_count -= 1;
    }
    const local_role role = role_of(capture_name(&analysis->locals, taken->capture.index));
    if (role == SXML_SOURCE_CODE_LOCAL_ROLE_SCOPE) {
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        const bool sets_inherits = find_property(
            (pattern_property){.compiled = &analysis->locals,
                               .pattern_index = taken->found.match->pattern_index,
                               .key = "local.scope-inherits"},
            &value);
        const local_scope scope = {
            .end_byte = ts_node_end_byte(node),
            .inherits = !sets_inherits ||
                        !byte_views_equal(value, SXML_BYTE_VIEW_FROM_LITERAL("false")),
            .first_definition = walk->definitions.element_count};
        return push_value(&walk->scopes, &scope, sizeof(scope));
    }
    if (role == SXML_SOURCE_CODE_LOCAL_ROLE_DEFINITION) {
        const local_definition definition = {.node_identity = (uintptr_t)node.id,
                                             .start_byte = start,
                                             .end_byte = ts_node_end_byte(node)};
        if (definition.end_byte > analysis->request->source_text.byte_count) {
            return SXML_STATUS_OK;
        }
        /* A definition is local too: "#is-not? local" patterns do not
           apply to it (tree-sitter's highlight crate). */
        const local_reference itself = {.reference_identity = definition.node_identity,
                                        .definition_identity = definition.node_identity};
        const sxml_status status =
            push_value(&analysis->local_references, &itself, sizeof(itself));
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return push_value(&walk->definitions, &definition, sizeof(definition));
    }
    if (role == SXML_SOURCE_CODE_LOCAL_ROLE_REFERENCE) {
        return resolve_reference(walk, node);
    }
    return SXML_STATUS_OK;
}

static int compare_local_references(const void *first, const void *second) {
    const local_reference *first_reference = first;
    const local_reference *second_reference = second;
    if (first_reference->reference_identity != second_reference->reference_identity) {
        return first_reference->reference_identity < second_reference->reference_identity ? -1 : 1;
    }
    return 0;
}

/* Runs the locals query over root and records the local references. */
static sxml_status run_locals(tree_analysis *analysis, TSNode root) {
    TSQueryCursorOptions options;
    TSQueryCursor *cursor = start_query(
        analysis, (query_start){.compiled = &analysis->locals, .options = &options}, root);
    if (cursor == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    const sxml_allocator allocator = analysis->request->allocator;
    locals_walk walk = {
        .analysis = analysis,
        .scopes = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(local_scope)}),
        .definitions = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(local_definition)})};
    const local_scope whole_tree = {.end_byte = UINT32_MAX, .inherits = false, .first_definition = 0};
    sxml_status status = push_value(&walk.scopes, &whole_tree, sizeof(whole_tree));
    TSQueryMatch match;
    uint32_t capture_index = 0;
    while (status == SXML_STATUS_OK &&
           ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
        bool holds = true;
        const query_match found = {.compiled = &analysis->locals, .match = &match};
        status = match_holds(analysis, found, &holds);
        if (status == SXML_STATUS_OK && holds) {
            status = take_local_capture(
                &walk, &(locals_capture){.found = found, .capture = match.captures[capture_index]});
        }
        if (status == SXML_STATUS_OK && !count_steps(analysis->account, 1)) {
            status = analysis->account->status;
        }
    }
    ts_query_cursor_delete(cursor);
    sxml_xml_array_release(&walk.scopes);
    sxml_xml_array_release(&walk.definitions);
    if (status == SXML_STATUS_OK) {
        status = settle_steps(analysis->account);
    }
    if (status == SXML_STATUS_OK && analysis->local_references.element_count > 1) {
        qsort(analysis->local_references.elements, analysis->local_references.element_count,
              sizeof(local_reference), compare_local_references);
        /* A node that is both a definition and a reference stays a
           definition. */
        local_reference *entries = sxml_xml_array_at(&analysis->local_references, 0);
        size_t kept_count = 1;
        for (size_t entry_index = 1; entry_index < analysis->local_references.element_count;
             entry_index += 1) {
            if (entries[entry_index].reference_identity !=
                entries[kept_count - 1].reference_identity) {
                entries[kept_count] = entries[entry_index];
                kept_count += 1;
            } else if (entries[entry_index].reference_identity ==
                       entries[entry_index].definition_identity) {
                entries[kept_count - 1] = entries[entry_index];
            }
        }
        analysis->local_references.element_count = kept_count;
    }
    return status;
}

static bool find_local_definition(const tree_analysis *analysis, uintptr_t identity,
                                  uintptr_t *definition_identity) {
    const size_t count = analysis->local_references.element_count;
    if (count == 0) {
        return false;
    }
    const local_reference *entries = sxml_xml_array_at(&analysis->local_references, 0);
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (entries[middle].reference_identity < identity) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == count || entries[low].reference_identity != identity) {
        return false;
    }
    *definition_identity = entries[low].definition_identity;
    return true;
}

/* ----------------------------------------------------------- Highlights */

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
static void keep_last_captures(sxml_xml_array *captures) {
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

/* The capture of a node, or NULL. */
static highlight_capture *find_capture(const tree_analysis *analysis, uintptr_t identity) {
    const size_t count = analysis->captures.element_count;
    if (count == 0) {
        return NULL;
    }
    highlight_capture *entries = sxml_xml_array_at(&analysis->captures, 0);
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (entries[middle].node_identity < identity) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < count && entries[low].node_identity == identity ? &entries[low] : NULL;
}

/* A local reference takes the highlight of its definition (tree-sitter's
   highlight crate). */
static sxml_status apply_local_highlights(tree_analysis *analysis) {
    const size_t reference_count = analysis->local_references.element_count;
    const size_t capture_count = analysis->captures.element_count;
    for (size_t reference_index = 0; reference_index < reference_count; reference_index += 1) {
        const local_reference *reference =
            sxml_xml_array_at(&analysis->local_references, reference_index);
        const highlight_capture *definition = NULL;
        /* Only the sorted part is searched: the entries pushed below are
           references, not definitions. */
        const size_t pushed_count = analysis->captures.element_count;
        analysis->captures.element_count = capture_count;
        definition = find_capture(analysis, reference->definition_identity);
        highlight_capture *own = find_capture(analysis, reference->reference_identity);
        analysis->captures.element_count = pushed_count;
        if (definition == NULL) {
            continue;
        }
        const highlight_capture taken = {.node_identity = reference->reference_identity,
                                         .pattern_index = definition->pattern_index,
                                         .capture_index = definition->capture_index};
        if (own != NULL) {
            *own = taken;
            continue;
        }
        const sxml_status status = push_value(&analysis->captures, &taken, sizeof(taken));
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (analysis->captures.element_count > capture_count) {
        qsort(analysis->captures.elements, analysis->captures.element_count,
              sizeof(highlight_capture), compare_captures);
    }
    return SXML_STATUS_OK;
}

static sxml_status record_match(tree_analysis *analysis, const TSQueryMatch *match) {
    for (uint16_t index = 0; index < match->capture_count; index += 1) {
        const TSQueryCapture capture = match->captures[index];
        if (!analysis->is_highlight_capture[capture.index]) {
            continue;
        }
        const highlight_capture taken = {.node_identity = (uintptr_t)capture.node.id,
                                         .pattern_index = match->pattern_index,
                                         .capture_index = capture.index};
        const sxml_status status = push_value(&analysis->captures, &taken, sizeof(taken));
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* Runs the highlights query over root and records the highlights. */
static sxml_status run_highlights(tree_analysis *analysis, TSNode root) {
    sxml_status status = SXML_STATUS_OK;
    TSQueryCursorOptions options;
    TSQueryCursor *cursor = start_query(
        analysis, (query_start){.compiled = &analysis->highlights, .options = &options}, root);
    if (cursor == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    TSQueryMatch match;
    while (status == SXML_STATUS_OK && ts_query_cursor_next_match(cursor, &match)) {
        bool holds = true;
        status = match_holds(analysis,
                             (query_match){.compiled = &analysis->highlights, .match = &match},
                             &holds);
        if (status == SXML_STATUS_OK && holds) {
            status = record_match(analysis, &match);
        }
        if (status == SXML_STATUS_OK && !count_steps(analysis->account, 1)) {
            status = analysis->account->status;
        }
    }
    ts_query_cursor_delete(cursor);
    if (status == SXML_STATUS_OK) {
        status = settle_steps(analysis->account);
    }
    if (status == SXML_STATUS_OK) {
        keep_last_captures(&analysis->captures);
        status = apply_local_highlights(analysis);
    }
    return status;
}

/* The highlight of node: its capture's name, or the empty view. */
static sxml_byte_view find_highlight(const tree_analysis *analysis, TSNode node) {
    if (analysis->highlights.query == NULL) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    const highlight_capture *capture = find_capture(analysis, (uintptr_t)node.id);
    if (capture == NULL) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return capture_name(&analysis->highlights, capture->capture_index);
}

/* ----------------------------------------------------------- Injections */

static int compare_injection_sites(const void *first, const void *second) {
    const injection_site *first_site = first;
    const injection_site *second_site = second;
    if (first_site->node_identity != second_site->node_identity) {
        return first_site->node_identity < second_site->node_identity ? -1 : 1;
    }
    return 0;
}

/* Records the injection of one match: its content node and language. */
static sxml_status record_injection(tree_analysis *analysis, const TSQueryMatch *match) {
    const compiled_query *compiled = &analysis->injections;
    bool has_content = false;
    TSNode content;
    memset(&content, 0, sizeof(content));
    sxml_byte_view language_name = {.bytes = NULL, .byte_count = 0};
    for (uint16_t index = 0; index < match->capture_count; index += 1) {
        const sxml_byte_view name = capture_name(compiled, match->captures[index].index);
        if (byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("injection.content")) &&
            !has_content) {
            has_content = true;
            content = match->captures[index].node;
        } else if (byte_views_equal(name, SXML_BYTE_VIEW_FROM_LITERAL("injection.language"))) {
            language_name = node_text(analysis, match->captures[index].node);
        }
    }
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    const pattern_property property = {
        .compiled = compiled, .pattern_index = match->pattern_index, .key = "injection.language"};
    if (find_property(property, &value)) {
        language_name = value;
    }
    pattern_property self = property;
    self.key = "injection.self";
    if (find_property(self, &value)) {
        language_name = analysis->language.language_name;
    }
    if (!has_content || language_name.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    pattern_property children = property;
    children.key = "injection.include-children";
    const injection_site site = {.node_identity = (uintptr_t)content.id,
                                 .language_name = language_name,
                                 .includes_children = find_property(children, &value)};
    return push_value(&analysis->injection_sites, &site, sizeof(site));
}

/* Runs the injections query over root and records the injection sites. */
static sxml_status run_injections(tree_analysis *analysis, TSNode root) {
    TSQueryCursorOptions options;
    TSQueryCursor *cursor = start_query(
        analysis, (query_start){.compiled = &analysis->injections, .options = &options}, root);
    if (cursor == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    sxml_status status = SXML_STATUS_OK;
    TSQueryMatch match;
    while (status == SXML_STATUS_OK && ts_query_cursor_next_match(cursor, &match)) {
        bool holds = true;
        status = match_holds(analysis,
                             (query_match){.compiled = &analysis->injections, .match = &match},
                             &holds);
        if (status == SXML_STATUS_OK && holds) {
            status = record_injection(analysis, &match);
        }
        if (status == SXML_STATUS_OK && !count_steps(analysis->account, 1)) {
            status = analysis->account->status;
        }
    }
    ts_query_cursor_delete(cursor);
    if (status == SXML_STATUS_OK) {
        status = settle_steps(analysis->account);
    }
    sxml_xml_array *sites = &analysis->injection_sites;
    if (status == SXML_STATUS_OK && sites->element_count > 1) {
        /* One site per node: the first match's (qsort keeps no order, so
           equal nodes are merged after sorting by node and match). */
        qsort(sites->elements, sites->element_count, sizeof(injection_site),
              compare_injection_sites);
        injection_site *entries = sxml_xml_array_at(sites, 0);
        size_t kept_count = 1;
        for (size_t entry_index = 1; entry_index < sites->element_count; entry_index += 1) {
            if (entries[entry_index].node_identity != entries[kept_count - 1].node_identity) {
                entries[kept_count] = entries[entry_index];
                kept_count += 1;
            }
        }
        sites->element_count = kept_count;
    }
    return status;
}

static const injection_site *find_injection_site(const tree_analysis *analysis, TSNode node) {
    const size_t count = analysis->injection_sites.element_count;
    if (count == 0) {
        return NULL;
    }
    const injection_site *entries = sxml_xml_array_at(&analysis->injection_sites, 0);
    const uintptr_t identity = (uintptr_t)node.id;
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (entries[middle].node_identity < identity) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < count && entries[low].node_identity == identity ? &entries[low] : NULL;
}

/* ------------------------------------------------------------- Analysis */

static tree_analysis empty_analysis(const sxml_source_code_request *request, step_account *account,
                                    sxml_source_code_language language) {
    const sxml_allocator allocator = request->allocator;
    const compiled_query none = {.query = NULL, .regexes = NULL, .regex_count = 0};
    return (tree_analysis){
        .request = request,
        .account = account,
        .language = language,
        .highlights = none,
        .locals = none,
        .injections = none,
        .is_highlight_capture = NULL,
        .captures = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(highlight_capture)}),
        .local_references = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(local_reference)}),
        .injection_sites = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(injection_site)})};
}

static void finalize_language_queries(sxml_allocator allocator, language_queries *queries) {
    finalize_query(allocator, &queries->highlights);
    finalize_query(allocator, &queries->locals);
    finalize_query(allocator, &queries->injections);
    sxml_allocator_release(allocator, queries->is_highlight_capture);
    queries->is_highlight_capture = NULL;
}

/* Marks the captures of the highlights query that are highlights: the
   names that do not start with an underscore. */
static sxml_status mark_highlight_captures(const tree_analysis *analysis,
                                           language_queries *queries) {
    const uint32_t capture_count = ts_query_capture_count(queries->highlights.query);
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        analysis->request->allocator,
        (sxml_array_allocation_request){.element_count = capture_count,
                                        .element_size = sizeof(bool),
                                        .element_alignment = alignof(bool)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    queries->is_highlight_capture = memory;
    for (uint32_t capture_index = 0; capture_index < capture_count; capture_index += 1) {
        const sxml_byte_view name = capture_name(&queries->highlights, capture_index);
        queries->is_highlight_capture[capture_index] = name.byte_count > 0 && name.bytes[0] != '_';
    }
    return SXML_STATUS_OK;
}

/* Where find_language_queries looks: the document's compiled queries
   (language_queries), and whether the language is injected. */
typedef struct queries_lookup {
    sxml_xml_array *cache;
    bool is_injected;
} queries_lookup;

/* Compiles the queries the request needs for the analysis's language. A
   query that does not compile gives SXML_STATUS_MALFORMED_INPUT for the
   document's language; for an injected language (lookup.is_injected) it
   is left out. */
static sxml_status compile_language_queries(const tree_analysis *analysis, queries_lookup lookup,
                                            language_queries *compiled) {
    const sxml_source_code_request *request = analysis->request;
    const sxml_source_code_language *language = &analysis->language;
    const struct {
        bool is_wanted;
        sxml_byte_view query_text;
        compiled_query *compiled;
    } queries[] = {
        {request->has_highlights, language->highlights_query, &compiled->highlights},
        {request->has_highlights, language->locals_query, &compiled->locals},
        {request->has_injections, language->injections_query, &compiled->injections},
    };
    sxml_status status = SXML_STATUS_OK;
    for (size_t query_index = 0;
         status == SXML_STATUS_OK && query_index < sizeof(queries) / sizeof(queries[0]);
         query_index += 1) {
        if (!queries[query_index].is_wanted) {
            continue;
        }
        status = compile_query(analysis, queries[query_index].query_text,
                               queries[query_index].compiled);
        if (status == SXML_STATUS_MALFORMED_INPUT && lookup.is_injected) {
            finalize_query(request->allocator, queries[query_index].compiled);
            status = SXML_STATUS_OK;
        }
    }
    if (status == SXML_STATUS_OK && compiled->highlights.query != NULL) {
        status = mark_highlight_captures(analysis, compiled);
    }
    return status;
}

/* The compiled queries of the analysis's language: the ones the document
   compiled before, or compiled now and kept for the rest of it. */
static sxml_status find_language_queries(const tree_analysis *analysis, queries_lookup lookup,
                                         const language_queries **found) {
    const sxml_source_code_language *language = &analysis->language;
    for (size_t entry_index = 0; entry_index < lookup.cache->element_count; entry_index += 1) {
        const language_queries *entry = sxml_xml_array_at(lookup.cache, entry_index);
        if (entry->grammar == language->grammar &&
            entry->highlights_query_bytes == language->highlights_query.bytes) {
            *found = entry;
            return SXML_STATUS_OK;
        }
    }
    const compiled_query none = {.query = NULL, .regexes = NULL, .regex_count = 0};
    language_queries compiled = {.grammar = language->grammar,
                                 .highlights_query_bytes = language->highlights_query.bytes,
                                 .highlights = none,
                                 .locals = none,
                                 .injections = none,
                                 .is_highlight_capture = NULL};
    sxml_status status = compile_language_queries(analysis, lookup, &compiled);
    if (status == SXML_STATUS_OK) {
        status = push_value(lookup.cache, &compiled, sizeof(compiled));
    }
    if (status != SXML_STATUS_OK) {
        finalize_language_queries(analysis->request->allocator, &compiled);
        return status;
    }
    *found = sxml_xml_array_at(lookup.cache, lookup.cache->element_count - 1);
    return SXML_STATUS_OK;
}

/* Takes the language's compiled queries and runs them over root. */
typedef struct analysis_request {
    TSNode root;
    bool is_injected;
    /* language_queries: the document's compiled queries. */
    sxml_xml_array *query_cache;
} analysis_request;

static sxml_status analyze_tree(tree_analysis *analysis, analysis_request analyzed) {
    const language_queries *queries = NULL;
    sxml_status status = find_language_queries(
        analysis, (queries_lookup){.cache = analyzed.query_cache, .is_injected = analyzed.is_injected},
        &queries);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    analysis->highlights = queries->highlights;
    analysis->locals = queries->locals;
    analysis->injections = queries->injections;
    analysis->is_highlight_capture = queries->is_highlight_capture;
    if (status == SXML_STATUS_OK && analysis->locals.query != NULL &&
        analysis->highlights.query != NULL) {
        status = run_locals(analysis, analyzed.root);
    }
    if (status == SXML_STATUS_OK && analysis->highlights.query != NULL) {
        status = run_highlights(analysis, analyzed.root);
    }
    if (status == SXML_STATUS_OK && analysis->injections.query != NULL) {
        status = run_injections(analysis, analyzed.root);
    }
    return status;
}

/* Releases what the analysis found; its queries belong to the document. */
static void finalize_analysis(tree_analysis *analysis) {
    analysis->is_highlight_capture = NULL;
    sxml_xml_array_release(&analysis->captures);
    sxml_xml_array_release(&analysis->local_references);
    sxml_xml_array_release(&analysis->injection_sites);
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
    SXML_SOURCE_CODE_ATTRIBUTE_LANGUAGE,
    SXML_SOURCE_CODE_ATTRIBUTE_LINE,
    SXML_SOURCE_CODE_ATTRIBUTE_COLUMN,
    SXML_SOURCE_CODE_ATTRIBUTE_END_LINE,
    SXML_SOURCE_CODE_ATTRIBUTE_END_COLUMN,
    SXML_SOURCE_CODE_ATTRIBUTE_COUNT,
};

static const char *const attribute_local_names[SXML_SOURCE_CODE_ATTRIBUTE_COUNT] = {
    [SXML_SOURCE_CODE_ATTRIBUTE_FIELD] = "field",
    [SXML_SOURCE_CODE_ATTRIBUTE_HIGHLIGHT] = "highlight",
    [SXML_SOURCE_CODE_ATTRIBUTE_MISSING] = "missing",
    [SXML_SOURCE_CODE_ATTRIBUTE_KIND] = "kind",
    [SXML_SOURCE_CODE_ATTRIBUTE_LANGUAGE] = "language",
    [SXML_SOURCE_CODE_ATTRIBUTE_LINE] = "line",
    [SXML_SOURCE_CODE_ATTRIBUTE_COLUMN] = "column",
    [SXML_SOURCE_CODE_ATTRIBUTE_END_LINE] = "end-line",
    [SXML_SOURCE_CODE_ATTRIBUTE_END_COLUMN] = "end-column",
};

/* The state of cached element names, by symbol. */
enum {
    SXML_SOURCE_CODE_KIND_NAME_UNKNOWN = 0,
    SXML_SOURCE_CODE_KIND_NAME_INTERNED = 1,
    SXML_SOURCE_CODE_KIND_NAME_NOT_NCNAME = 2,
};

/* Where the walk of a layer is. */
typedef enum layer_step {
    /* The root's element is started; its children are next. */
    SXML_SOURCE_CODE_LAYER_STEP_START,
    /* The cursor is on a node not written yet. */
    SXML_SOURCE_CODE_LAYER_STEP_VISIT,
    /* The cursor's node is written; its next sibling or an ancestor's is
       next. */
    SXML_SOURCE_CODE_LAYER_STEP_CLIMB,
    /* Every node below the root is written. */
    SXML_SOURCE_CODE_LAYER_STEP_FINISH,
} layer_step;

/* One tree being written: the document's, or an injection's above the
   layer of its node. */
typedef struct code_layer {
    TSTree *tree;
    TSTreeCursor cursor;
    tree_analysis analysis;
    layer_step next_step;
    /* The open nodes below the root are open_nodes' entries from here. */
    size_t open_node_base;
    bool root_makes_element;
    /* The end of the layer's text: the whole text for the document's
       layer, the injection node's end for an injection's. */
    uint32_t end_byte;
    /* For an injection's layer: its node in the layer below, and whether
       that node made an element, to end when this layer ends. */
    bool has_host;
    TSNode host_node;
    bool host_makes_element;
    /* By symbol below symbol_count: the interned kind and its state. */
    sxml_name_identifier *kind_names;
    uint8_t *kind_name_states;
    uint32_t symbol_count;
} code_layer;

typedef struct document_writer {
    const sxml_source_code_request *request;
    step_account *account;
    sxml_name_identifier attribute_names[SXML_SOURCE_CODE_ATTRIBUTE_COUNT];
    sxml_name_identifier token_name;
    sxml_name_identifier kind_element_name;
    sxml_namespace_prefix_identifier sxml_prefix;
    /* The source bytes written as text so far. */
    size_t written_byte_count;
    /* By open node below a layer's root: whether it made an element
       (bool). */
    sxml_xml_array open_nodes;
    /* code_layer, the document's first (D71: the stack of trees). */
    sxml_xml_array layers;
    /* language_queries, one for each language the document uses. */
    sxml_xml_array language_queries;
} document_writer;

static code_layer *top_layer(const document_writer *writer) {
    return sxml_xml_array_at(&writer->layers, writer->layers.element_count - 1);
}

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
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < SXML_SOURCE_CODE_ATTRIBUTE_COUNT;
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
    return status;
}

/* Allocates the layer's cache of element names by symbol. */
static sxml_status prepare_kind_names(const document_writer *writer, code_layer *layer) {
    const sxml_allocator allocator = writer->request->allocator;
    layer->symbol_count = ts_language_symbol_count(layer->analysis.language.grammar);
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = layer->symbol_count,
                                        .element_size = sizeof(sxml_name_identifier),
                                        .element_alignment = alignof(sxml_name_identifier)},
        &memory);
    layer->kind_names = memory;
    memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = layer->symbol_count,
                                            .element_size = sizeof(uint8_t),
                                            .element_alignment = alignof(uint8_t)},
            &memory);
    }
    layer->kind_name_states = memory;
    if (status == SXML_STATUS_OK && layer->symbol_count > 0) {
        memset(layer->kind_name_states, SXML_SOURCE_CODE_KIND_NAME_UNKNOWN, layer->symbol_count);
    }
    return status;
}

/* Releases what a layer holds: its cursor, tree, analysis and cache. */
static void finalize_layer(const document_writer *writer, code_layer *layer) {
    const sxml_allocator allocator = writer->request->allocator;
    sxml_allocator_release(allocator, layer->kind_names);
    sxml_allocator_release(allocator, layer->kind_name_states);
    layer->kind_names = NULL;
    layer->kind_name_states = NULL;
    finalize_analysis(&layer->analysis);
    ts_tree_cursor_delete(&layer->cursor);
    ts_tree_delete(layer->tree);
    layer->tree = NULL;
}

/* The element name of a named node's kind; is_ncname false when the kind
   is no NCName (the node becomes an sxml:node element). */
typedef struct kind_name {
    sxml_name_identifier name;
    bool is_ncname;
} kind_name;

/* A node of a layer. */
typedef struct layer_node {
    code_layer *layer;
    TSNode node;
} layer_node;

static sxml_status find_kind_name(document_writer *writer, layer_node visited, kind_name *found) {
    code_layer *layer = visited.layer;
    const TSSymbol symbol = ts_node_symbol(visited.node);
    const bool is_cached = symbol < layer->symbol_count;
    if (is_cached && layer->kind_name_states[symbol] != SXML_SOURCE_CODE_KIND_NAME_UNKNOWN) {
        found->is_ncname = layer->kind_name_states[symbol] == SXML_SOURCE_CODE_KIND_NAME_INTERNED;
        found->name = layer->kind_names[symbol];
        return SXML_STATUS_OK;
    }
    const sxml_byte_view kind = sxml_byte_view_from_c_string(ts_node_type(visited.node));
    found->is_ncname = sxml_xml_text_is_ncname(kind);
    found->name = writer->kind_element_name;
    sxml_status status = SXML_STATUS_OK;
    if (found->is_ncname) {
        status = intern_local_name(writer, kind, &found->name);
    }
    if (status == SXML_STATUS_OK && is_cached) {
        layer->kind_name_states[symbol] = found->is_ncname ? SXML_SOURCE_CODE_KIND_NAME_INTERNED
                                                           : SXML_SOURCE_CODE_KIND_NAME_NOT_NCNAME;
        layer->kind_names[symbol] = found->name;
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
    code_layer *layer;
    TSNode node;
    sxml_byte_view field_name;
    bool is_document_element;
    /* An injection's document element: its language, and the node of the
       layer below whose place it has. */
    sxml_byte_view language_name;
    TSNode place_node;
} node_visit;

static node_shape shape_of(const node_visit *visit, sxml_byte_view highlight) {
    if (ts_node_is_named(visit->node)) {
        return SXML_SOURCE_CODE_NODE_SHAPE_ELEMENT;
    }
    if (ts_node_is_missing(visit->node) || highlight.byte_count > 0 ||
        visit->field_name.byte_count > 0 || visit->language_name.byte_count > 0) {
        return SXML_SOURCE_CODE_NODE_SHAPE_TOKEN;
    }
    return SXML_SOURCE_CODE_NODE_SHAPE_TEXT;
}

static sxml_status write_positions(document_writer *writer, const node_visit *visit) {
    const TSNode node = visit->language_name.byte_count > 0 ? visit->place_node : visit->node;
    const source_place start =
        visit->is_document_element
            ? (source_place){.line = 1, .column = 1}
            : place_of(writer, ts_node_start_point(node), ts_node_start_byte(node));
    const source_place end =
        visit->is_document_element
            ? text_end_place(writer)
            : place_of(writer, ts_node_end_point(node), ts_node_end_byte(node));
    sxml_status status = write_place(writer, start, SXML_SOURCE_CODE_ATTRIBUTE_LINE);
    if (status == SXML_STATUS_OK) {
        status = write_place(writer, end, SXML_SOURCE_CODE_ATTRIBUTE_END_LINE);
    }
    return status;
}

/* Starts the element of a node that makes one, with its attributes;
   *makes_element false for a node that is text. */
static sxml_status start_node(document_writer *writer, const node_visit *visit,
                              bool *makes_element) {
    const TSNode node = visit->node;
    const sxml_byte_view highlight = find_highlight(&visit->layer->analysis, node);
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
        status = find_kind_name(writer, (layer_node){.layer = visit->layer, .node = node}, &found);
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
        status = write_attribute(writer, SXML_SOURCE_CODE_ATTRIBUTE_MISSING,
                                 SXML_BYTE_VIEW_FROM_LITERAL("true"));
    }
    if (status == SXML_STATUS_OK &&
        (shape == SXML_SOURCE_CODE_NODE_SHAPE_KIND_ELEMENT ||
         (shape == SXML_SOURCE_CODE_NODE_SHAPE_TOKEN && is_missing))) {
        status = write_attribute(writer, SXML_SOURCE_CODE_ATTRIBUTE_KIND,
                                 sxml_byte_view_from_c_string(ts_node_type(node)));
    }
    if (status == SXML_STATUS_OK && visit->language_name.byte_count > 0) {
        status = write_attribute(writer, SXML_SOURCE_CODE_ATTRIBUTE_LANGUAGE, visit->language_name);
    }
    if (status == SXML_STATUS_OK && writer->request->has_positions) {
        status = write_positions(writer, visit);
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
    return push_value(&writer->open_nodes, &makes_element, sizeof(makes_element));
}

static bool pop_open_node(document_writer *writer) {
    writer->open_nodes.element_count -= 1;
    return *(const bool *)sxml_xml_array_at(&writer->open_nodes, writer->open_nodes.element_count);
}

/* ------------------------------------------------------------- Layers */

/* A new layer for a parsed tree: its analysis, its name cache, and its
   root's element started. The layer is on top of the stack even on
   failure, so that it is released with the others. */
typedef struct layer_start {
    TSTree *tree;
    sxml_source_code_language language;
    bool is_injected;
    uint32_t end_byte;
    /* For an injection: its node in the layer below. */
    TSNode host_node;
    bool host_makes_element;
} layer_start;

static sxml_status push_layer(document_writer *writer, const layer_start *start) {
    const TSNode root = ts_tree_root_node(start->tree);
    code_layer layer = {
        .tree = start->tree,
        .cursor = ts_tree_cursor_new(root),
        .analysis = empty_analysis(writer->request, writer->account, start->language),
        .next_step = SXML_SOURCE_CODE_LAYER_STEP_START,
        .open_node_base = writer->open_nodes.element_count,
        .root_makes_element = false,
        .end_byte = start->end_byte,
        .has_host = start->is_injected,
        .host_node = start->host_node,
        .host_makes_element = start->host_makes_element,
        .kind_names = NULL,
        .kind_name_states = NULL,
        .symbol_count = 0};
    sxml_status status = push_value(&writer->layers, &layer, sizeof(layer));
    if (status != SXML_STATUS_OK) {
        finalize_layer(writer, &layer);
        return status;
    }
    code_layer *pushed = top_layer(writer);
    status = analyze_tree(&pushed->analysis,
                          (analysis_request){.root = root,
                                             .is_injected = start->is_injected,
                                             .query_cache = &writer->language_queries});
    if (status == SXML_STATUS_OK) {
        status = prepare_kind_names(writer, pushed);
    }
    if (status == SXML_STATUS_OK) {
        status = write_text(writer, ts_node_start_byte(root));
    }
    if (status == SXML_STATUS_OK) {
        status = start_node(
            writer,
            &(node_visit){.layer = pushed,
                          .node = root,
                          .field_name = {.bytes = NULL, .byte_count = 0},
                          .is_document_element = !start->is_injected,
                          .language_name = start->is_injected
                                               ? start->language.language_name
                                               : (sxml_byte_view){.bytes = NULL, .byte_count = 0},
                          .place_node = start->host_node},
            &pushed->root_makes_element);
    }
    return status;
}

/* Finds the language an injection names: by its name, else by the first
   language whose injection regex matches it (registered ones first). */
typedef struct language_search {
    document_writer *writer;
    sxml_byte_view language_name;
} language_search;

static sxml_status regex_names_language(language_search search, sxml_byte_view regex_text,
                                        bool *names) {
    *names = false;
    if (regex_text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    regex_pair pair = {.regex = NULL, .matcher = NULL};
    sxml_status status = compile_regex(search.writer->request->allocator, regex_text, &pair);
    if (status == SXML_STATUS_OK && pair.matcher != NULL) {
        uint64_t step_count = 0;
        status = regex_matches(
            (regex_search){.matcher = pair.matcher, .text = search.language_name}, &step_count,
            names);
        if (status == SXML_STATUS_OK && !count_steps(search.writer->account, step_count)) {
            status = search.writer->account->status;
        }
    }
    release_regex(&pair);
    return status;
}

static sxml_status find_injected_language(language_search search,
                                          sxml_source_code_language *language, bool *is_found) {
    const sxml_source_code_language_set *set = &search.writer->request->language_set;
    *is_found = sxml_source_code_find_language(set, search.language_name, language);
    if (*is_found) {
        return SXML_STATUS_OK;
    }
    const sxml_source_code_language_view registered = set->registered_languages;
    sxml_status status = SXML_STATUS_OK;
    for (size_t language_index = 0;
         status == SXML_STATUS_OK && !*is_found && language_index < registered.language_count;
         language_index += 1) {
        status = regex_names_language(
            search, registered.languages[language_index].injection_regex, is_found);
        if (*is_found) {
            *language = registered.languages[language_index];
        }
    }
    for (size_t language_index = 0;
         status == SXML_STATUS_OK && !*is_found &&
         language_index < sxml_source_code_builtin_languages.language_count;
         language_index += 1) {
        const sxml_source_code_builtin_language *builtin =
            &sxml_source_code_builtin_languages.languages[language_index];
        status = regex_names_language(search, builtin->injection_regex, is_found);
        /* A registered language of the same name replaces the built-in
           one. */
        if (*is_found) {
            *is_found = sxml_source_code_find_language(set, builtin->language_name, language);
        }
    }
    return status;
}

/* The ranges of the injection's text: the node's, without its named
   children's unless the injection includes them (tree-sitter's included
   ranges; named children, as Neovim and tree-sitter's highlight crate
   take them: Markdown's inline nodes have anonymous tokens of the block
   grammar inside, which belong to the inline text). */
typedef struct injection_text {
    const injection_site *site;
    TSNode node;
} injection_text;

static sxml_status collect_ranges(document_writer *writer, injection_text text,
                                  sxml_xml_array *ranges) {
    const injection_site *site = text.site;
    const TSNode node = text.node;
    TSRange current = {.start_point = ts_node_start_point(node),
                       .end_point = ts_node_end_point(node),
                       .start_byte = ts_node_start_byte(node),
                       .end_byte = ts_node_end_byte(node)};
    const uint32_t child_count = site->includes_children ? 0 : ts_node_named_child_count(node);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t child_index = 0; status == SXML_STATUS_OK && child_index < child_count;
         child_index += 1) {
        const TSNode child = ts_node_named_child(node, child_index);
        TSRange before = current;
        before.end_byte = ts_node_start_byte(child);
        before.end_point = ts_node_start_point(child);
        if (before.end_byte > before.start_byte) {
            status = push_value(ranges, &before, sizeof(before));
        }
        current.start_byte = ts_node_end_byte(child);
        current.start_point = ts_node_end_point(child);
        if (status == SXML_STATUS_OK && !count_steps(writer->account, 1)) {
            status = writer->account->status;
        }
    }
    if (status == SXML_STATUS_OK && current.end_byte > current.start_byte) {
        status = push_value(ranges, &current, sizeof(current));
    }
    return status;
}

/* Parses the node's text with the injection's language and pushes its
   tree as a layer; *is_pushed false when the language is unknown or the
   text empty, and the node is written as any other. */
typedef struct injection_start {
    const injection_site *site;
    TSNode node;
    bool makes_element;
} injection_start;

static sxml_status push_injection(document_writer *writer, const injection_start *start,
                                  bool *is_pushed) {
    *is_pushed = false;
    sxml_source_code_language language;
    bool is_found = false;
    sxml_status status = find_injected_language(
        (language_search){.writer = writer, .language_name = start->site->language_name},
        &language, &is_found);
    if (status != SXML_STATUS_OK || !is_found || language.grammar == NULL) {
        return status;
    }
    sxml_xml_array ranges = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = writer->request->allocator, .element_byte_count = sizeof(TSRange)});
    status = collect_ranges(writer, (injection_text){.site = start->site, .node = start->node},
                            &ranges);
    TSTree *tree = NULL;
    if (status == SXML_STATUS_OK && ranges.element_count > 0) {
        status = parse_tree(
            &(parse_job){.grammar = language.grammar,
                         .source_text = writer->request->source_text,
                         .ranges = sxml_xml_array_at(&ranges, 0),
                         .range_count = (uint32_t)ranges.element_count},
            writer->account, &tree);
        /* A grammar of another ABI is no language to inject. */
        if (status == SXML_STATUS_INVALID_ARGUMENT) {
            status = SXML_STATUS_OK;
        }
    }
    sxml_xml_array_release(&ranges);
    if (status != SXML_STATUS_OK || tree == NULL) {
        return status;
    }
    *is_pushed = true;
    return push_layer(writer, &(layer_start){.tree = tree,
                                             .language = language,
                                             .is_injected = true,
                                             .end_byte = ts_node_end_byte(start->node),
                                             .host_node = start->node,
                                             .host_makes_element = start->makes_element});
}

/* Writes the node under the top layer's cursor: its element, then an
   injection's layer, its children, or its end. */
static sxml_status visit_node(document_writer *writer) {
    code_layer *layer = top_layer(writer);
    const TSNode node = ts_tree_cursor_current_node(&layer->cursor);
    const char *field_name = ts_tree_cursor_current_field_name(&layer->cursor);
    sxml_status status = write_text(writer, ts_node_start_byte(node));
    bool makes_element = false;
    if (status == SXML_STATUS_OK) {
        status = start_node(
            writer,
            &(node_visit){.layer = layer,
                          .node = node,
                          .field_name = field_name == NULL
                                            ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                            : sxml_byte_view_from_c_string(field_name),
                          .is_document_element = false,
                          .language_name = {.bytes = NULL, .byte_count = 0},
                          .place_node = node},
            &makes_element);
    }
    if (status == SXML_STATUS_OK && !count_steps(writer->account, 1)) {
        status = writer->account->status;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const injection_site *site =
        writer->layers.element_count <= SXML_SOURCE_CODE_MAXIMUM_INJECTION_DEPTH
            ? find_injection_site(&layer->analysis, node)
            : NULL;
    if (site != NULL) {
        /* The node's subtree is the injection's: after it, the next
           sibling. */
        layer->next_step = SXML_SOURCE_CODE_LAYER_STEP_CLIMB;
        bool is_pushed = false;
        status = push_injection(
            writer, &(injection_start){.site = site, .node = node, .makes_element = makes_element},
            &is_pushed);
        if (status != SXML_STATUS_OK || is_pushed) {
            return status;
        }
        layer = top_layer(writer);
        layer->next_step = SXML_SOURCE_CODE_LAYER_STEP_VISIT;
    }
    if (ts_tree_cursor_goto_first_child(&layer->cursor)) {
        return push_open_node(writer, makes_element);
    }
    layer->next_step = SXML_SOURCE_CODE_LAYER_STEP_CLIMB;
    return end_node(writer, node, makes_element);
}

/* Moves the top layer's cursor to the next node to write, ending the
   nodes left behind. */
static sxml_status climb(document_writer *writer) {
    code_layer *layer = top_layer(writer);
    while (!ts_tree_cursor_goto_next_sibling(&layer->cursor)) {
        if (writer->open_nodes.element_count == layer->open_node_base) {
            layer->next_step = SXML_SOURCE_CODE_LAYER_STEP_FINISH;
            return SXML_STATUS_OK;
        }
        (void)ts_tree_cursor_goto_parent(&layer->cursor);
        const bool parent_makes_element = pop_open_node(writer);
        const sxml_status status = end_node(
            writer, ts_tree_cursor_current_node(&layer->cursor), parent_makes_element);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    layer->next_step = SXML_SOURCE_CODE_LAYER_STEP_VISIT;
    return SXML_STATUS_OK;
}

/* Ends the top layer's root, pops the layer and ends an injection's node. */
static sxml_status finish_layer(document_writer *writer) {
    code_layer *layer = top_layer(writer);
    sxml_status status = write_text(writer, layer->end_byte);
    if (status == SXML_STATUS_OK && layer->root_makes_element) {
        const sxml_tree_builder builder = writer->request->builder;
        status = builder.operations->end_element(builder.state);
    }
    const bool has_host = layer->has_host;
    const TSNode host_node = layer->host_node;
    const bool host_makes_element = layer->host_makes_element;
    finalize_layer(writer, layer);
    writer->layers.element_count -= 1;
    if (status == SXML_STATUS_OK && has_host) {
        status = end_node(writer, host_node, host_makes_element);
    }
    return status;
}

/* Writes the layers until the document's is done (D71: one loop). */
static sxml_status write_layers(document_writer *writer) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && writer->layers.element_count > 0) {
        code_layer *layer = top_layer(writer);
        switch (layer->next_step) {
        case SXML_SOURCE_CODE_LAYER_STEP_START:
            layer->next_step = ts_tree_cursor_goto_first_child(&layer->cursor)
                                   ? SXML_SOURCE_CODE_LAYER_STEP_VISIT
                                   : SXML_SOURCE_CODE_LAYER_STEP_FINISH;
            break;
        case SXML_SOURCE_CODE_LAYER_STEP_VISIT:
            status = visit_node(writer);
            break;
        case SXML_SOURCE_CODE_LAYER_STEP_CLIMB:
            status = climb(writer);
            break;
        case SXML_SOURCE_CODE_LAYER_STEP_FINISH:
            status = finish_layer(writer);
            break;
        }
    }
    return status;
}

/* ----------------------------------------------------------------- Parse */

sxml_status sxml_source_code_parse(const sxml_source_code_request *request) {
    if (request == NULL || request->language.grammar == NULL ||
        request->builder.operations == NULL || request->name_pool == NULL ||
        (request->source_text.bytes == NULL && request->source_text.byte_count > 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->source_text.byte_count >= UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    step_account account = {
        .meter = request->step_meter, .pending_step_count = 0, .status = SXML_STATUS_OK};
    TSTree *tree = NULL;
    sxml_status status = parse_tree(&(parse_job){.grammar = request->language.grammar,
                                                 .source_text = request->source_text,
                                                 .ranges = NULL,
                                                 .range_count = 0},
                                    &account, &tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    document_writer writer = {
        .request = request,
        .account = &account,
        .attribute_names = {{.index = 0}},
        .token_name = {.index = 0},
        .kind_element_name = {.index = 0},
        .sxml_prefix = {.index = 0},
        .written_byte_count = 0,
        .open_nodes = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = request->allocator, .element_byte_count = sizeof(bool)}),
        .layers = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = request->allocator, .element_byte_count = sizeof(code_layer)}),
        .language_queries = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = request->allocator, .element_byte_count = sizeof(language_queries)})};
    status = prepare_writer(&writer);
    const sxml_tree_builder builder = request->builder;
    if (status == SXML_STATUS_OK) {
        status = builder.operations->start_document(
            builder.state,
            &(sxml_document_start_event){.document_uri = {.bytes = NULL, .byte_count = 0},
                                         .base_uri = {.bytes = NULL, .byte_count = 0},
                                         .location = {.line_number = 0}});
    }
    if (status == SXML_STATUS_OK) {
        TSNode no_host;
        memset(&no_host, 0, sizeof(no_host));
        status = push_layer(&writer, &(layer_start){.tree = tree,
                                                    .language = request->language,
                                                    .is_injected = false,
                                                    .end_byte = (uint32_t)request->source_text.byte_count,
                                                    .host_node = no_host,
                                                    .host_makes_element = false});
        tree = NULL;
    }
    if (status == SXML_STATUS_OK) {
        status = write_layers(&writer);
    }
    if (status == SXML_STATUS_OK) {
        status = settle_steps(&account);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    for (size_t layer_index = writer.layers.element_count; layer_index > 0; layer_index -= 1) {
        finalize_layer(&writer, sxml_xml_array_at(&writer.layers, layer_index - 1));
    }
    sxml_xml_array_release(&writer.layers);
    for (size_t entry_index = 0; entry_index < writer.language_queries.element_count;
         entry_index += 1) {
        finalize_language_queries(request->allocator,
                                  sxml_xml_array_at(&writer.language_queries, entry_index));
    }
    sxml_xml_array_release(&writer.language_queries);
    sxml_xml_array_release(&writer.open_nodes);
    if (tree != NULL) {
        ts_tree_delete(tree);
    }
    return status;
}
