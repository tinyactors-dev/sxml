/*
 * External scanner shared by the XPath 4.0, XQuery 4.0 and XSLT 4.0 pattern
 * grammars (docs/DECISIONS.md, D29).
 *
 * Each grammars/<language>/src/scanner.c includes this file and defines the
 * five tree-sitter entry points for its language with
 * SXML_QUERY_SCANNER_DEFINE(<language>, <has direct constructors>). The
 * scanner keeps no state between tokens: the set of valid tokens tells it where it is.
 *
 * It recognizes the terminals a context-free tokenizer cannot:
 * - nested comments "(: ... :)" (XPath 4.0 A.3.1 Comment, gn: comments);
 * - the fixed parts of string templates and string constructors
 *   (XPath 4.0 StringTemplateFixedPart, XQuery 4.0 StringConstructorChars);
 * - direct constructors (XQuery 4.0 3.9.1 and A.3: the "<" rules), whose
 *   start tags, attribute values and content are ws:explicit;
 * - pragmas "(#" S EQName (S PragmaContents)? "#)" (XQuery 4.0 3.20).
 *
 * The scanner never allocates. tree-sitter calls it through fixed callback
 * signatures, which have at most three parameters.
 */
#ifndef SXML_GRAMMARS_COMMON_QUERY_SCANNER_H
#define SXML_GRAMMARS_COMMON_QUERY_SCANNER_H

#include <stdbool.h>
#include <stdint.h>

#include "tree_sitter/parser.h"

/* The order matches the `externals` list in grammars/xpath/grammar.js. */
enum query_token_kind {
    QUERY_TOKEN_COMMENT,
    QUERY_TOKEN_STRING_TEMPLATE_FIXED_PART,
    QUERY_TOKEN_STRING_CONSTRUCTOR_CHARACTERS,
    QUERY_TOKEN_DIRECT_ELEMENT_OPEN,
    QUERY_TOKEN_DIRECT_TAG_NAME,
    QUERY_TOKEN_DIRECT_ATTRIBUTE_NAME,
    QUERY_TOKEN_DIRECT_ATTRIBUTE_EQUALS,
    QUERY_TOKEN_DIRECT_QUOTE_OPEN,
    QUERY_TOKEN_DIRECT_QUOTE_CLOSE,
    QUERY_TOKEN_DIRECT_APOSTROPHE_OPEN,
    QUERY_TOKEN_DIRECT_APOSTROPHE_CLOSE,
    QUERY_TOKEN_DIRECT_QUOTE_CONTENT,
    QUERY_TOKEN_DIRECT_APOSTROPHE_CONTENT,
    QUERY_TOKEN_DIRECT_TAG_END,
    QUERY_TOKEN_DIRECT_EMPTY_TAG_END,
    QUERY_TOKEN_DIRECT_END_TAG_OPEN,
    QUERY_TOKEN_DIRECT_ELEMENT_TEXT,
    QUERY_TOKEN_DIRECT_COMMENT_CONSTRUCTOR,
    QUERY_TOKEN_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR,
    QUERY_TOKEN_CDATA_SECTION,
    QUERY_TOKEN_PRAGMA_OPEN,
    QUERY_TOKEN_PRAGMA_CONTENTS,
    QUERY_TOKEN_ERROR_SENTINEL,
};

/* XML 1.0 [3] S. */
static bool query_scanner_is_whitespace(int32_t character)
{
    return character == 0x20 || character == 0x09 || character == 0x0D || character == 0x0A;
}

/* XML 1.0 [4] NameStartChar, without ":" (XML Names NCName). */
static bool query_scanner_is_name_start(int32_t character)
{
    return (character >= 'A' && character <= 'Z') || character == '_'
        || (character >= 'a' && character <= 'z')
        || (character >= 0xC0 && character <= 0xD6)
        || (character >= 0xD8 && character <= 0xF6)
        || (character >= 0xF8 && character <= 0x2FF)
        || (character >= 0x370 && character <= 0x37D)
        || (character >= 0x37F && character <= 0x1FFF)
        || (character >= 0x200C && character <= 0x200D)
        || (character >= 0x2070 && character <= 0x218F)
        || (character >= 0x2C00 && character <= 0x2FEF)
        || (character >= 0x3001 && character <= 0xD7FF)
        || (character >= 0xF900 && character <= 0xFDCF)
        || (character >= 0xFDF0 && character <= 0xFFFD)
        || (character >= 0x10000 && character <= 0xEFFFF);
}

/* XML 1.0 [4a] NameChar, without ":". */
static bool query_scanner_is_name_character(int32_t character)
{
    return query_scanner_is_name_start(character) || character == '-' || character == '.'
        || (character >= '0' && character <= '9') || character == 0xB7
        || (character >= 0x300 && character <= 0x36F)
        || (character >= 0x203F && character <= 0x2040);
}

/* XML 1.0 [2] Char. */
static bool query_scanner_is_character(int32_t character)
{
    return character == 0x09 || character == 0x0A || character == 0x0D
        || (character >= 0x20 && character <= 0xD7FF)
        || (character >= 0xE000 && character <= 0xFFFD)
        || (character >= 0x10000 && character <= 0x10FFFF);
}

static bool query_scanner_is_at_end(TSLexer *lexer)
{
    return lexer->eof(lexer);
}

static void query_scanner_advance(TSLexer *lexer)
{
    lexer->advance(lexer, false);
}

static void query_scanner_skip_whitespace(TSLexer *lexer)
{
    while (!query_scanner_is_at_end(lexer) && query_scanner_is_whitespace(lexer->lookahead)) {
        lexer->advance(lexer, true);
    }
}

static bool query_scanner_accept(TSLexer *lexer, enum query_token_kind kind)
{
    lexer->result_symbol = (TSSymbol)kind;
    return true;
}

/* Consumes an NCName; returns false (consuming nothing) when none starts here. */
static bool query_scanner_consume_ncname(TSLexer *lexer)
{
    if (query_scanner_is_at_end(lexer) || !query_scanner_is_name_start(lexer->lookahead)) {
        return false;
    }
    while (!query_scanner_is_at_end(lexer) && query_scanner_is_name_character(lexer->lookahead)) {
        query_scanner_advance(lexer);
    }
    return true;
}

/*
 * Consumes a lexical QName (XML Names [7]): NCName (":" NCName)?.
 * Returns false when the input is not a QName; the token is then rejected.
 */
static bool query_scanner_consume_qname(TSLexer *lexer)
{
    if (!query_scanner_consume_ncname(lexer)) {
        return false;
    }
    if (lexer->lookahead == ':') {
        query_scanner_advance(lexer);
        return query_scanner_consume_ncname(lexer);
    }
    return true;
}

/* Consumes a Name (XML 1.0 [5]), which may contain any number of colons. */
static bool query_scanner_consume_name(TSLexer *lexer)
{
    if (query_scanner_is_at_end(lexer)
        || !(query_scanner_is_name_start(lexer->lookahead) || lexer->lookahead == ':')) {
        return false;
    }
    while (!query_scanner_is_at_end(lexer)
           && (query_scanner_is_name_character(lexer->lookahead) || lexer->lookahead == ':')) {
        query_scanner_advance(lexer);
    }
    return true;
}

/*
 * Consumes the rest of a comment after "(:", honouring nesting
 * (XPath 4.0 A.1.3 gn: comments). Returns false if the input ends first.
 */
static bool query_scanner_consume_comment_rest(TSLexer *lexer)
{
    uint32_t depth = 1;
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        query_scanner_advance(lexer);
        if (character == '(' && lexer->lookahead == ':') {
            query_scanner_advance(lexer);
            depth += 1;
        } else if (character == ':' && lexer->lookahead == ')') {
            query_scanner_advance(lexer);
            depth -= 1;
            if (depth == 0) {
                return true;
            }
        }
    }
    return false;
}

/*
 * Consumes a predefined entity reference or a character reference after
 * "&" has been seen but not consumed (XQuery 4.0 PredefinedEntityRef,
 * XML 1.0 [66] CharRef). Returns false on a malformed reference.
 */
static bool query_scanner_consume_reference(TSLexer *lexer)
{
    query_scanner_advance(lexer);
    if (lexer->lookahead == '#') {
        query_scanner_advance(lexer);
        bool is_hexadecimal = false;
        if (lexer->lookahead == 'x') {
            is_hexadecimal = true;
            query_scanner_advance(lexer);
        }
        uint32_t digit_count = 0;
        for (;;) {
            int32_t character = lexer->lookahead;
            bool is_digit = character >= '0' && character <= '9';
            bool is_hexadecimal_letter = (character >= 'a' && character <= 'f')
                || (character >= 'A' && character <= 'F');
            if (!(is_digit || (is_hexadecimal && is_hexadecimal_letter))) {
                break;
            }
            query_scanner_advance(lexer);
            digit_count += 1;
        }
        if (digit_count == 0 || lexer->lookahead != ';') {
            return false;
        }
        query_scanner_advance(lexer);
        return true;
    }
    static const char *const entity_names[] = {"lt", "gt", "amp", "quot", "apos"};
    char name[8];
    uint32_t name_length_in_bytes = 0;
    while (lexer->lookahead >= 'a' && lexer->lookahead <= 'z' && name_length_in_bytes < 7) {
        name[name_length_in_bytes] = (char)lexer->lookahead;
        name_length_in_bytes += 1;
        query_scanner_advance(lexer);
    }
    name[name_length_in_bytes] = '\0';
    if (lexer->lookahead != ';') {
        return false;
    }
    for (uint32_t entity_index = 0; entity_index < 5; entity_index += 1) {
        const char *entity_name = entity_names[entity_index];
        uint32_t character_index = 0;
        while (entity_name[character_index] != '\0'
               && entity_name[character_index] == name[character_index]) {
            character_index += 1;
        }
        if (entity_name[character_index] == '\0' && name[character_index] == '\0') {
            query_scanner_advance(lexer);
            return true;
        }
    }
    return false;
}

/*
 * After "<" (consumed, token end marked): checks the A.3 rules that make
 * "<" the start of a DirElemConstructor:
 *   "<" Name S? ">",  "<" Name S? "/>",  "<" Name S Name S? "="
 * (the regular expressions ^<\i\c*\s*> and so on in the specification).
 * The characters looked at here are not part of the token.
 */
static bool query_scanner_starts_direct_element(TSLexer *lexer)
{
    if (!query_scanner_consume_name(lexer)) {
        return false;
    }
    bool has_whitespace = false;
    while (!query_scanner_is_at_end(lexer) && query_scanner_is_whitespace(lexer->lookahead)) {
        has_whitespace = true;
        query_scanner_advance(lexer);
    }
    if (lexer->lookahead == '>') {
        return true;
    }
    if (lexer->lookahead == '/') {
        query_scanner_advance(lexer);
        return lexer->lookahead == '>';
    }
    if (!has_whitespace || !query_scanner_consume_name(lexer)) {
        return false;
    }
    while (!query_scanner_is_at_end(lexer) && query_scanner_is_whitespace(lexer->lookahead)) {
        query_scanner_advance(lexer);
    }
    return lexer->lookahead == '=';
}

/* After "<!--": DirCommentContents "-->" (XQuery 4.0 3.9.3.3). */
static bool query_scanner_consume_direct_comment_rest(TSLexer *lexer)
{
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (!query_scanner_is_character(character)) {
            return false;
        }
        query_scanner_advance(lexer);
        if (character == '-' && lexer->lookahead == '-') {
            query_scanner_advance(lexer);
            if (lexer->lookahead != '>') {
                return false;
            }
            query_scanner_advance(lexer);
            return true;
        }
    }
    return false;
}

/*
 * After "<?": PITarget (S DirPIContents)? "?>" (XQuery 4.0 3.9.3.2).
 * PITarget is a Name other than "xml" in any case (XML 1.0 [17]); with
 * namespaces it is also an NCName (XML Names 1.0, section 7).
 */
static bool query_scanner_consume_direct_processing_instruction_rest(TSLexer *lexer)
{
    static const char reserved_target[] = "xml";
    uint32_t target_length = 0;
    bool matches_reserved_target = true;
    if (query_scanner_is_at_end(lexer) || !query_scanner_is_name_start(lexer->lookahead)) {
        return false;
    }
    while (!query_scanner_is_at_end(lexer) && query_scanner_is_name_character(lexer->lookahead)) {
        int32_t character = lexer->lookahead;
        int32_t lower_case = character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        if (target_length >= 3 || lower_case != reserved_target[target_length]) {
            matches_reserved_target = false;
        }
        target_length += 1;
        query_scanner_advance(lexer);
    }
    if (matches_reserved_target && target_length == 3) {
        return false;
    }
    if (lexer->lookahead == '?') {
        query_scanner_advance(lexer);
        if (lexer->lookahead != '>') {
            return false;
        }
        query_scanner_advance(lexer);
        return true;
    }
    if (!query_scanner_is_whitespace(lexer->lookahead)) {
        return false;
    }
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (!query_scanner_is_character(character)) {
            return false;
        }
        query_scanner_advance(lexer);
        if (character == '?' && lexer->lookahead == '>') {
            query_scanner_advance(lexer);
            return true;
        }
    }
    return false;
}

/* After "<![CDATA[": CDataSectionContents "]]>" (XQuery 4.0 3.9.1.1). */
static bool query_scanner_consume_cdata_rest(TSLexer *lexer)
{
    uint32_t closing_bracket_count = 0;
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (!query_scanner_is_character(character)) {
            return false;
        }
        query_scanner_advance(lexer);
        if (character == '>' && closing_bracket_count >= 2) {
            return true;
        }
        closing_bracket_count = character == ']' ? closing_bracket_count + 1 : 0;
    }
    return false;
}

static bool query_scanner_consume_literal(TSLexer *lexer, const char *literal)
{
    for (const char *character = literal; *character != '\0'; character += 1) {
        if (lexer->lookahead != *character) {
            return false;
        }
        query_scanner_advance(lexer);
    }
    return true;
}

/*
 * At "<" where a constructor may start: a direct element, comment or
 * processing-instruction constructor, or (in element content) also an end
 * tag or a CDATA section. Implements the "<" rules of XQuery 4.0 A.3.
 */
static bool query_scanner_scan_less_than(TSLexer *lexer, const bool *valid_symbols)
{
    /*
     * A.3: the "<" analysis does not depend on the syntactic context, so a
     * constructor is recognized even where the grammar expects the operator
     * "<"; the parser then reports the syntax error.
     */
    query_scanner_advance(lexer);
    lexer->mark_end(lexer);
    int32_t character = lexer->lookahead;
    if (character == '/' && valid_symbols[QUERY_TOKEN_DIRECT_END_TAG_OPEN]) {
        query_scanner_advance(lexer);
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_END_TAG_OPEN);
    }
    if (character == '!') {
        query_scanner_advance(lexer);
        if (lexer->lookahead == '-') {
            if (!query_scanner_consume_literal(lexer, "--")
                || !query_scanner_consume_direct_comment_rest(lexer)) {
                return false;
            }
            lexer->mark_end(lexer);
            return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_COMMENT_CONSTRUCTOR);
        }
        if (lexer->lookahead == '[' && valid_symbols[QUERY_TOKEN_CDATA_SECTION]) {
            if (!query_scanner_consume_literal(lexer, "[CDATA[")
                || !query_scanner_consume_cdata_rest(lexer)) {
                return false;
            }
            lexer->mark_end(lexer);
            return query_scanner_accept(lexer, QUERY_TOKEN_CDATA_SECTION);
        }
        return false;
    }
    if (character == '?') {
        query_scanner_advance(lexer);
        if (!query_scanner_consume_direct_processing_instruction_rest(lexer)) {
            return false;
        }
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR);
    }
    if (query_scanner_starts_direct_element(lexer)) {
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_ELEMENT_OPEN);
    }
    return false;
}

/*
 * Element content (XQuery 4.0 3.9.1.3): ElementContentChar, references and
 * "{{" / "}}" form one text token; "<" starts markup; a single "{" starts an
 * enclosed expression and is left to the grammar.
 */
static bool query_scanner_scan_element_content(TSLexer *lexer, const bool *valid_symbols)
{
    if (lexer->lookahead == '<') {
        return query_scanner_scan_less_than(lexer, valid_symbols);
    }
    bool has_text = false;
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == '<') {
            break;
        }
        if (character == '{' || character == '}') {
            query_scanner_advance(lexer);
            if (lexer->lookahead != character) {
                break;
            }
            query_scanner_advance(lexer);
        } else if (character == '&') {
            if (!query_scanner_consume_reference(lexer)) {
                break;
            }
        } else if (!query_scanner_is_character(character)) {
            break;
        } else {
            query_scanner_advance(lexer);
        }
        has_text = true;
        lexer->mark_end(lexer);
    }
    return has_text && query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_ELEMENT_TEXT);
}

/*
 * Attribute value content (XQuery 4.0 3.9.1.1): QuotAttrValueContent or
 * AposAttrValueContent, with the doubled delimiter as an escape.
 */
static bool query_scanner_scan_attribute_value(TSLexer *lexer, const bool *valid_symbols,
                                               int32_t delimiter)
{
    enum query_token_kind content_kind = delimiter == '"' ? QUERY_TOKEN_DIRECT_QUOTE_CONTENT
                                                          : QUERY_TOKEN_DIRECT_APOSTROPHE_CONTENT;
    enum query_token_kind close_kind = delimiter == '"' ? QUERY_TOKEN_DIRECT_QUOTE_CLOSE
                                                        : QUERY_TOKEN_DIRECT_APOSTROPHE_CLOSE;
    bool has_text = false;
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == delimiter) {
            query_scanner_advance(lexer);
            if (lexer->lookahead != delimiter) {
                if (has_text) {
                    break;
                }
                lexer->mark_end(lexer);
                return valid_symbols[close_kind] && query_scanner_accept(lexer, close_kind);
            }
            query_scanner_advance(lexer);
        } else if (character == '{' || character == '}') {
            query_scanner_advance(lexer);
            if (lexer->lookahead != character) {
                break;
            }
            query_scanner_advance(lexer);
        } else if (character == '&') {
            if (!query_scanner_consume_reference(lexer)) {
                break;
            }
        } else if (character == '<' || !query_scanner_is_character(character)) {
            break;
        } else {
            query_scanner_advance(lexer);
        }
        has_text = true;
        lexer->mark_end(lexer);
    }
    return has_text && valid_symbols[content_kind] && query_scanner_accept(lexer, content_kind);
}

/*
 * Inside a start or end tag: names, "=", quotes and the tag ends. White
 * space here is XML S; comments are not allowed (A.1.3 gn: comments).
 */
static bool query_scanner_scan_tag(TSLexer *lexer, const bool *valid_symbols)
{
    if (valid_symbols[QUERY_TOKEN_DIRECT_TAG_NAME]) {
        if (!query_scanner_consume_qname(lexer)) {
            return false;
        }
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_TAG_NAME);
    }
    bool has_whitespace = !query_scanner_is_at_end(lexer) && query_scanner_is_whitespace(lexer->lookahead);
    query_scanner_skip_whitespace(lexer);
    int32_t character = lexer->lookahead;
    if (character == '>' && valid_symbols[QUERY_TOKEN_DIRECT_TAG_END]) {
        query_scanner_advance(lexer);
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_TAG_END);
    }
    if (character == '/' && valid_symbols[QUERY_TOKEN_DIRECT_EMPTY_TAG_END]) {
        query_scanner_advance(lexer);
        if (lexer->lookahead != '>') {
            return false;
        }
        query_scanner_advance(lexer);
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_EMPTY_TAG_END);
    }
    if (character == '=' && valid_symbols[QUERY_TOKEN_DIRECT_ATTRIBUTE_EQUALS]) {
        query_scanner_advance(lexer);
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_ATTRIBUTE_EQUALS);
    }
    if (character == '"' && valid_symbols[QUERY_TOKEN_DIRECT_QUOTE_OPEN]) {
        query_scanner_advance(lexer);
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_QUOTE_OPEN);
    }
    if (character == '\'' && valid_symbols[QUERY_TOKEN_DIRECT_APOSTROPHE_OPEN]) {
        query_scanner_advance(lexer);
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_APOSTROPHE_OPEN);
    }
    /* DirAttributeList ::= (S (QName S? "=" S? DirAttributeValue)?)* */
    if (has_whitespace && valid_symbols[QUERY_TOKEN_DIRECT_ATTRIBUTE_NAME]
        && query_scanner_consume_qname(lexer)) {
        lexer->mark_end(lexer);
        return query_scanner_accept(lexer, QUERY_TOKEN_DIRECT_ATTRIBUTE_NAME);
    }
    return false;
}

/*
 * StringTemplateFixedPart ::= ((Char - ('{' | '}' | '`')) | "{{" | "}}" | "``")+
 * (XPath 4.0 4.9.2).
 */
static bool query_scanner_scan_string_template(TSLexer *lexer)
{
    bool has_text = false;
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == '{' || character == '}' || character == '`') {
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
    return has_text && query_scanner_accept(lexer, QUERY_TOKEN_STRING_TEMPLATE_FIXED_PART);
}

/*
 * StringConstructorChars ::= (Char* - (Char* ('`{' | ']``') Char*))
 * (XQuery 4.0 3.10.2). Ends before "`{" or "]``".
 */
static bool query_scanner_scan_string_constructor(TSLexer *lexer)
{
    bool has_text = false;
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (!query_scanner_is_character(character)) {
            break;
        }
        query_scanner_advance(lexer);
        if (character == '`' && lexer->lookahead == '{') {
            break;
        }
        if (character == ']' && lexer->lookahead == '`') {
            query_scanner_advance(lexer);
            if (lexer->lookahead == '`') {
                break;
            }
        }
        has_text = true;
        lexer->mark_end(lexer);
    }
    return has_text && query_scanner_accept(lexer, QUERY_TOKEN_STRING_CONSTRUCTOR_CHARACTERS);
}

/* PragmaContents ::= (Char* - (Char* '#)' Char*)), after the S that precedes it. */
static bool query_scanner_scan_pragma_contents(TSLexer *lexer)
{
    if (query_scanner_is_at_end(lexer) || !query_scanner_is_whitespace(lexer->lookahead)) {
        return false;
    }
    while (!query_scanner_is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (!query_scanner_is_character(character)) {
            return false;
        }
        if (character == '#') {
            lexer->mark_end(lexer);
            query_scanner_advance(lexer);
            if (lexer->lookahead == ')') {
                return query_scanner_accept(lexer, QUERY_TOKEN_PRAGMA_CONTENTS);
            }
        } else {
            query_scanner_advance(lexer);
        }
    }
    return false;
}

/* What distinguishes the languages that share this scanner. */
typedef struct query_scanner_options {
    /* XQuery: "<" may start a direct constructor (XQuery 4.0 A.3). */
    bool recognizes_direct_constructors;
} query_scanner_options;

static bool query_scanner_scan(TSLexer *lexer, const bool *valid_symbols,
                               const query_scanner_options *options)
{
    /* During error recovery every token is valid; produce nothing. */
    if (valid_symbols[QUERY_TOKEN_ERROR_SENTINEL]) {
        return false;
    }
    if (valid_symbols[QUERY_TOKEN_DIRECT_ELEMENT_TEXT]) {
        return query_scanner_scan_element_content(lexer, valid_symbols);
    }
    if (valid_symbols[QUERY_TOKEN_DIRECT_QUOTE_CONTENT] || valid_symbols[QUERY_TOKEN_DIRECT_QUOTE_CLOSE]) {
        return query_scanner_scan_attribute_value(lexer, valid_symbols, '"');
    }
    if (valid_symbols[QUERY_TOKEN_DIRECT_APOSTROPHE_CONTENT]
        || valid_symbols[QUERY_TOKEN_DIRECT_APOSTROPHE_CLOSE]) {
        return query_scanner_scan_attribute_value(lexer, valid_symbols, '\'');
    }
    if (valid_symbols[QUERY_TOKEN_STRING_TEMPLATE_FIXED_PART]) {
        return query_scanner_scan_string_template(lexer);
    }
    if (valid_symbols[QUERY_TOKEN_STRING_CONSTRUCTOR_CHARACTERS]) {
        return query_scanner_scan_string_constructor(lexer);
    }
    if (valid_symbols[QUERY_TOKEN_DIRECT_TAG_NAME] || valid_symbols[QUERY_TOKEN_DIRECT_ATTRIBUTE_NAME]
        || valid_symbols[QUERY_TOKEN_DIRECT_ATTRIBUTE_EQUALS]
        || valid_symbols[QUERY_TOKEN_DIRECT_QUOTE_OPEN] || valid_symbols[QUERY_TOKEN_DIRECT_TAG_END]
        || valid_symbols[QUERY_TOKEN_DIRECT_EMPTY_TAG_END]) {
        return query_scanner_scan_tag(lexer, valid_symbols);
    }
    /* Pragma ::= "(#" S EQName (S PragmaContents)? "#)" is ws:explicit. */
    if (valid_symbols[QUERY_TOKEN_PRAGMA_CONTENTS]) {
        return query_scanner_scan_pragma_contents(lexer);
    }

    query_scanner_skip_whitespace(lexer);
    if (lexer->lookahead == '(') {
        query_scanner_advance(lexer);
        if (lexer->lookahead == ':' && valid_symbols[QUERY_TOKEN_COMMENT]) {
            query_scanner_advance(lexer);
            if (!query_scanner_consume_comment_rest(lexer)) {
                return false;
            }
            lexer->mark_end(lexer);
            return query_scanner_accept(lexer, QUERY_TOKEN_COMMENT);
        }
        /* XQuery 4.0 A.3: "(#" followed by whitespace starts a pragma. */
        if (lexer->lookahead == '#' && valid_symbols[QUERY_TOKEN_PRAGMA_OPEN]) {
            query_scanner_advance(lexer);
            if (!query_scanner_is_whitespace(lexer->lookahead)) {
                return false;
            }
            query_scanner_skip_whitespace(lexer);
            lexer->mark_end(lexer);
            /* No comment may stand for the S after "(#". */
            if (lexer->lookahead == '(') {
                query_scanner_advance(lexer);
                if (lexer->lookahead == ':') {
                    return false;
                }
            }
            return query_scanner_accept(lexer, QUERY_TOKEN_PRAGMA_OPEN);
        }
        return false;
    }
    if (lexer->lookahead == '<' && options->recognizes_direct_constructors) {
        return query_scanner_scan_less_than(lexer, valid_symbols);
    }
    return false;
}

/* Defines the tree-sitter external scanner entry points for one language. */
#define SXML_QUERY_SCANNER_DEFINE(language, has_direct_constructors)                                                     \
    void *tree_sitter_##language##_external_scanner_create(void)                                  \
    {                                                                                             \
        return NULL;                                                                              \
    }                                                                                             \
    void tree_sitter_##language##_external_scanner_destroy(void *payload)                         \
    {                                                                                             \
        (void)payload;                                                                            \
    }                                                                                             \
    unsigned tree_sitter_##language##_external_scanner_serialize(void *payload, char *buffer)     \
    {                                                                                             \
        (void)payload;                                                                            \
        (void)buffer;                                                                             \
        return 0;                                                                                 \
    }                                                                                             \
    void tree_sitter_##language##_external_scanner_deserialize(void *payload, const char *buffer, \
                                                               unsigned byte_count)               \
    {                                                                                             \
        (void)payload;                                                                            \
        (void)buffer;                                                                             \
        (void)byte_count;                                                                         \
    }                                                                                             \
    bool tree_sitter_##language##_external_scanner_scan(void *payload, TSLexer *lexer,            \
                                                        const bool *valid_symbols)                \
    {                                                                                             \
        (void)payload;                                                                            \
        return query_scanner_scan(                                                                \
            lexer, valid_symbols,                                                                 \
            &(query_scanner_options){.recognizes_direct_constructors = (has_direct_constructors)}); \
    }

#endif
