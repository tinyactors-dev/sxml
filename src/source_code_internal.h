#ifndef SXML_SOURCE_CODE_INTERNAL_H
#define SXML_SOURCE_CODE_INTERNAL_H

/*
 * Source code as XML (packet 7.22, D285; glossary `source_code_language`,
 * `syntax_node`, `token_element`): the bridge from tree-sitter syntax
 * trees to tree builder events, behind sxml:parse-code
 * (src/function_library_source_code.c), the optional feature
 * sxml_source_code_feature().
 *
 * A language is a tree-sitter grammar and its highlights, locals and
 * injections queries (glossary `highlights_query`, `locals_query`,
 * `injections_query`): one vendored under third_party/tree-sitter-grammars
 * (its generated parser, linked in), compiled in by
 * tools/generate-source-code-languages.py (src/source_code_languages.c),
 * or one the host registered (include/sxml/processor.h,
 * sxml_source_code_language; D288).
 *
 * The document (D285):
 * - one element per named syntax node, named by its kind (glossary
 *   `node_kind`) in no namespace; a kind that is no NCName makes an
 *   sxml:node element with the kind in its kind attribute;
 * - the document element is the root node's, and it holds the whole
 *   text, so the string value of the document is the source text;
 * - anonymous nodes (tokens) and the text between nodes are text, except
 *   that a token with a highlight or a field name becomes an sxml:token
 *   element (glossary `token_element`);
 * - attributes: field (glossary `field_name`), highlight (the capture
 *   of the last highlights pattern that captures the node, glossary
 *   `capture`), missing="true"
 *   with kind for a token the parser inserted (tree-sitter's MISSING),
 *   and with positions line, column, end-line and end-column (from 1,
 *   columns in characters); ERROR nodes are ERROR elements;
 * - sxml is bound to urn:sxml on the document element.
 *
 * tree-sitter's parser, tree, query and cursors allocate with malloc (the
 * D12 exemption, which D285 extends from the parse to the query step);
 * all of them are freed before sxml_source_code_parse returns. The walk
 * is one loop over a TSTreeCursor with an explicit stack in allocator
 * memory (D71). Parsing, the query and the walk charge their steps to the
 * request's step meter, and a step limit cancels the parse (its progress
 * callback) or the query.
 *
 * Thread safety: the caller's thread; the language table is static const.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tree_sitter/api.h>

/* One built-in language (src/source_code_languages.c): a vendored
   grammar, whose TSLanguage its entry point returns, and its queries. */
typedef struct sxml_source_code_builtin_language {
    /* ASCII, static: the name in the $language argument ("javascript"). */
    sxml_byte_view language_name;
    /* The grammar's generated entry point. */
    const TSLanguage *(*get_grammar)(void);
    /* The query texts (tree-sitter query syntax), static; empty for
       none. */
    sxml_byte_view highlights_query;
    sxml_byte_view locals_query;
    sxml_byte_view injections_query;
    /* tree-sitter.json's injection-regex, static; empty for none. */
    sxml_byte_view injection_regex;
} sxml_source_code_builtin_language;

typedef struct sxml_source_code_builtin_language_view {
    const sxml_source_code_builtin_language *languages;
    size_t language_count;
} sxml_source_code_builtin_language_view;

/* Every built-in language, in name order (src/source_code_languages.c). */
extern const sxml_source_code_builtin_language_view sxml_source_code_builtin_languages;

/* The built-in language at language_index as a language (its grammar from
   its entry point). */
sxml_source_code_language sxml_source_code_builtin_language_at(size_t language_index);

/* The languages of one processor: the registered ones, then the built-in
   ones they do not replace (glossary `registered_language`, D288). */
typedef struct sxml_source_code_language_set {
    sxml_source_code_language_view registered_languages;
} sxml_source_code_language_set;

/* Writes the language named name to *language; false when there is none.
   A registered language comes before a built-in one. */
bool sxml_source_code_find_language(const sxml_source_code_language_set *set,
                                    sxml_byte_view name, sxml_source_code_language *language);

/* Whether a registered language's name is good: not empty, ASCII
   letters, digits, '_', '-', '+', '.' (src/source_code_check.c). */
bool sxml_source_code_language_name_is_valid(sxml_byte_view name);

/* Whether a configuration's view of registered languages is good: an
   array for a count, valid names, grammars of a supported ABI, no name
   twice (sxml_processor_initialize; src/source_code_check.c). */
bool sxml_source_code_language_view_is_valid(sxml_source_code_language_view view);

/* Charges steps to the work's budget: SXML_STATUS_LIMIT_EXCEEDED past it
   (the evaluation's step limit, D89, D91). */
typedef struct sxml_source_code_step_meter_operations {
    sxml_status (*charge)(void *state, uint64_t step_count);
} sxml_source_code_step_meter_operations;

typedef struct sxml_source_code_step_meter {
    void *state;
    const sxml_source_code_step_meter_operations *operations;
} sxml_source_code_step_meter;

/* What to parse, and how to write it. */
typedef struct sxml_source_code_request {
    /* The language of the text. */
    sxml_source_code_language language;
    /* Where the languages of injections are found (D288). */
    sxml_source_code_language_set language_set;
    /* Well-formed UTF-8, borrowed for the call; below 4 GiB. */
    sxml_byte_view source_text;
    /* Run the highlights query and write highlight attributes. */
    bool has_highlights;
    /* Write line, column, end-line and end-column attributes. */
    bool has_positions;
    /* Parse the nodes the injections query names with their languages
       (glossary `injection`, D288). */
    bool has_injections;
    /* Receives the document: start_document to end_document. */
    sxml_tree_builder builder;
    sxml_name_pool *name_pool;
    /* The walk's stack, name cache and capture table. */
    sxml_allocator allocator;
    sxml_source_code_step_meter step_meter;
} sxml_source_code_request;

/*
 * Parses request->source_text with the language's grammar and sends the
 * document described above to request->builder.
 *
 * Returns SXML_STATUS_LIMIT_EXCEEDED for a text of 4 GiB or more and when
 * the step meter refuses a charge (the parse or query is cancelled),
 * SXML_STATUS_OUT_OF_MEMORY when tree-sitter cannot allocate,
 * SXML_STATUS_INVALID_ARGUMENT when the grammar's ABI does not fit the
 * runtime, SXML_STATUS_MALFORMED_INPUT when one of the language's
 * queries does not compile for its grammar (sxml:invalid-query; an
 * injected language's bad query only leaves its highlights out), and the
 * failures of the builder, the
 * name pool and the allocator. The builder may have received part of the
 * document on failure.
 */
SXML_NODISCARD sxml_status sxml_source_code_parse(const sxml_source_code_request *request);

#endif
