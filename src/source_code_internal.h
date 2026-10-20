#ifndef SXML_SOURCE_CODE_INTERNAL_H
#define SXML_SOURCE_CODE_INTERNAL_H

/*
 * Source code as XML (packet 7.22, D285; glossary `source_code_language`,
 * `syntax_node`, `token_element`): the bridge from tree-sitter syntax
 * trees to tree builder events, behind sxml:parse-code
 * (src/function_library_source_code.c), the optional feature
 * sxml_source_code_feature().
 *
 * A language is a tree-sitter grammar vendored under
 * third_party/tree-sitter-grammars (its generated parser, linked in) and
 * its highlights query (glossary `highlights_query`), compiled in by
 * tools/generate-source-code-languages.py (src/source_code_languages.c).
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
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tree_sitter/api.h>

/* One language sxml:parse-code knows. */
typedef struct sxml_source_code_language {
    /* ASCII, static: the name in the $language argument ("javascript"). */
    sxml_byte_view language_name;
    /* The grammar's generated entry point. */
    const TSLanguage *(*get_grammar)(void);
    /* The highlights query text (tree-sitter query syntax), static. */
    sxml_byte_view highlights_query;
} sxml_source_code_language;

typedef struct sxml_source_code_language_view {
    const sxml_source_code_language *languages;
    size_t language_count;
} sxml_source_code_language_view;

/* Every language, in name order (src/source_code_languages.c). */
extern const sxml_source_code_language_view sxml_source_code_languages;

/* The language named name, or NULL when there is none. */
const sxml_source_code_language *sxml_source_code_find_language(sxml_byte_view name);

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
    const sxml_source_code_language *language;
    /* Well-formed UTF-8, borrowed for the call; below 4 GiB. */
    sxml_byte_view source_text;
    /* Run the highlights query and write highlight attributes. */
    bool has_highlights;
    /* Write line, column, end-line and end-column attributes. */
    bool has_positions;
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
 * SXML_STATUS_INVALID_ARGUMENT when the grammar's ABI or query does not
 * fit the runtime (a build error), and the failures of the builder, the
 * name pool and the allocator. The builder may have received part of the
 * document on failure.
 */
SXML_NODISCARD sxml_status sxml_source_code_parse(const sxml_source_code_request *request);

#endif
