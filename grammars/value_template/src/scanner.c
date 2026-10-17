/*
 * External scanner for the value_template grammar (docs/DECISIONS.md, D29
 * and D97): the shared query scanner (grammars/common/query_scanner.h) for
 * the tokens of enclosed expressions, and two tokens of its own.
 */
#include "../../common/query_scanner.h"

/* After the shared tokens, in the order of `externals` in grammar.js. */
enum value_template_token_kind {
    VALUE_TEMPLATE_TOKEN_FIXED_PART = QUERY_TOKEN_ERROR_SENTINEL + 1,
    VALUE_TEMPLATE_TOKEN_START,
};

/*
 * A fixed part (XSLT 4.0 5.6): any characters, a left curly bracket written
 * as "{{" and a right one as "}}". A single "{" starts a variable part; a
 * single "}" is no token, so the parse fails there (XTSE0370).
 */
static bool value_template_scan_fixed_part(TSLexer *lexer)
{
    bool has_text = false;
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == '{' || character == '}') {
            query_scanner_advance(lexer);
            if (lexer->lookahead != character) {
                break;
            }
            query_scanner_advance(lexer);
        } else if (!query_scanner_is_character(character)) {
            break;
        } else {
            query_scanner_advance(lexer);
        }
        has_text = true;
        lexer->mark_end(lexer);
    }
    if (!has_text) {
        return false;
    }
    lexer->result_symbol = (TSSymbol)VALUE_TEMPLATE_TOKEN_FIXED_PART;
    return true;
}

void *tree_sitter_value_template_external_scanner_create(void)
{
    return NULL;
}

void tree_sitter_value_template_external_scanner_destroy(void *payload)
{
    (void)payload;
}

unsigned tree_sitter_value_template_external_scanner_serialize(void *payload, char *buffer)
{
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_value_template_external_scanner_deserialize(void *payload, const char *buffer,
                                                             unsigned byte_count)
{
    (void)payload;
    (void)buffer;
    (void)byte_count;
}

bool tree_sitter_value_template_external_scanner_scan(void *payload, TSLexer *lexer,
                                                      const bool *valid_symbols)
{
    (void)payload;
    /* During error recovery every token is valid; produce nothing. */
    if (valid_symbols[QUERY_TOKEN_ERROR_SENTINEL]) {
        return false;
    }
    if (valid_symbols[VALUE_TEMPLATE_TOKEN_START]) {
        lexer->mark_end(lexer);
        lexer->result_symbol = (TSSymbol)VALUE_TEMPLATE_TOKEN_START;
        return true;
    }
    /* Outside variable parts nothing else is a token: "(:" is text. */
    if (valid_symbols[VALUE_TEMPLATE_TOKEN_FIXED_PART]) {
        return value_template_scan_fixed_part(lexer);
    }
    return query_scanner_scan(lexer, valid_symbols,
                              &(query_scanner_options){.recognizes_direct_constructors = false});
}
