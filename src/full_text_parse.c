/*
 * The Full Text parser part (src/full_text_internal.h, "Parsing";
 * docs/DECISIONS.md D111, D214, D235): the node types, fields and tokens
 * that XQuery and XPath Full Text 3.0 adds to a grammar, defined once for
 * the two grammars that have its productions (grammars/xpath/full_text.js):
 * XQuery's parser part names it as its extension (src/xquery_parse.c), and
 * so does the XPath Full Text part of the optional feature full-text
 * (src/full_text.c), whose grammar is grammars/xpath_full_text.
 *
 * Golden file: src/xquery_parse.c (sorted tables, apply_token).
 *
 * Section numbers are of XQuery and XPath Full Text 3.0 (W3C
 * Recommendation, 24 November 2015).
 */
#include "full_text_internal.h"

#include "xpath_ast_internal.h"

/* ------------------------------------------------------------- Tables */

/*
 * The named node types of the Full Text productions, sorted in byte
 * order: every ft_* node type, and pragma and pragma_contents, which
 * FTExtensionSelection needs (3.6; XQuery's own table maps them too, for
 * its extension expressions). let_score_binding is XQuery's (2.3.2: XPath
 * has no score variables in let bindings).
 */
static const sxml_ast_node_type_entry full_text_node_types[] = {
    {SXML_BYTE_VIEW_INITIALIZER("ft_and"), SXML_AST_KIND_FT_AND},
    {SXML_BYTE_VIEW_INITIALIZER("ft_case_option"), SXML_AST_KIND_FT_CASE_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_contains_expression"),
     SXML_AST_KIND_FT_CONTAINS_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_content"), SXML_AST_KIND_FT_CONTENT},
    {SXML_BYTE_VIEW_INITIALIZER("ft_diacritics_option"), SXML_AST_KIND_FT_DIACRITICS_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_distance"), SXML_AST_KIND_FT_DISTANCE},
    {SXML_BYTE_VIEW_INITIALIZER("ft_extension_option"), SXML_AST_KIND_FT_EXTENSION_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_extension_selection"),
     SXML_AST_KIND_FT_EXTENSION_SELECTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_language_option"), SXML_AST_KIND_FT_LANGUAGE_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_mild_not"), SXML_AST_KIND_FT_MILD_NOT},
    {SXML_BYTE_VIEW_INITIALIZER("ft_option_declaration"),
     SXML_AST_KIND_FT_OPTION_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_or"), SXML_AST_KIND_FT_OR},
    {SXML_BYTE_VIEW_INITIALIZER("ft_order"), SXML_AST_KIND_FT_ORDER},
    {SXML_BYTE_VIEW_INITIALIZER("ft_primary_with_options"),
     SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("ft_range"), SXML_AST_KIND_FT_RANGE},
    {SXML_BYTE_VIEW_INITIALIZER("ft_scope"), SXML_AST_KIND_FT_SCOPE},
    {SXML_BYTE_VIEW_INITIALIZER("ft_score_variable"), SXML_AST_KIND_FT_SCORE_VARIABLE},
    {SXML_BYTE_VIEW_INITIALIZER("ft_selection"), SXML_AST_KIND_FT_SELECTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_stem_option"), SXML_AST_KIND_FT_STEM_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_stop_word_option"), SXML_AST_KIND_FT_STOP_WORD_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_stop_words"), SXML_AST_KIND_FT_STOP_WORDS},
    {SXML_BYTE_VIEW_INITIALIZER("ft_stop_words_inclusion"),
     SXML_AST_KIND_FT_STOP_WORDS_INCLUSION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_thesaurus_id"), SXML_AST_KIND_FT_THESAURUS_ID},
    {SXML_BYTE_VIEW_INITIALIZER("ft_thesaurus_option"), SXML_AST_KIND_FT_THESAURUS_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_unary_not"), SXML_AST_KIND_FT_UNARY_NOT},
    {SXML_BYTE_VIEW_INITIALIZER("ft_wildcard_option"), SXML_AST_KIND_FT_WILDCARD_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("ft_window"), SXML_AST_KIND_FT_WINDOW},
    {SXML_BYTE_VIEW_INITIALIZER("ft_words"), SXML_AST_KIND_FT_WORDS},
    {SXML_BYTE_VIEW_INITIALIZER("pragma"), SXML_AST_KIND_PRAGMA},
    {SXML_BYTE_VIEW_INITIALIZER("pragma_contents"), SXML_AST_KIND_PRAGMA_CONTENTS},
};

/*
 * The fields of the Full Text productions that hold named nodes and that
 * grammars/xpath does not have, sorted in byte order (contents and pragma
 * are XQuery's too). The fields that hold tokens (any_all, different, mode,
 * no, separate_words, unit, uses_default, and content of ft_content) are
 * read by apply_full_text_token.
 */
static const sxml_ast_field_entry full_text_fields[] = {
    {SXML_BYTE_VIEW_INITIALIZER("contents"), SXML_AST_FIELD_CONTENTS},
    {SXML_BYTE_VIEW_INITIALIZER("filter"), SXML_AST_FIELD_FILTER},
    {SXML_BYTE_VIEW_INITIALIZER("ignore"), SXML_AST_FIELD_IGNORE},
    {SXML_BYTE_VIEW_INITIALIZER("inclusion"), SXML_AST_FIELD_INCLUSION},
    {SXML_BYTE_VIEW_INITIALIZER("language"), SXML_AST_FIELD_LANGUAGE},
    {SXML_BYTE_VIEW_INITIALIZER("maximum"), SXML_AST_FIELD_MAXIMUM},
    {SXML_BYTE_VIEW_INITIALIZER("minimum"), SXML_AST_FIELD_MINIMUM},
    {SXML_BYTE_VIEW_INITIALIZER("option"), SXML_AST_FIELD_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("pragma"), SXML_AST_FIELD_PRAGMA},
    {SXML_BYTE_VIEW_INITIALIZER("range"), SXML_AST_FIELD_RANGE},
    {SXML_BYTE_VIEW_INITIALIZER("relationship"), SXML_AST_FIELD_RELATIONSHIP},
    {SXML_BYTE_VIEW_INITIALIZER("score"), SXML_AST_FIELD_SCORE},
    {SXML_BYTE_VIEW_INITIALIZER("selection"), SXML_AST_FIELD_SELECTION},
    {SXML_BYTE_VIEW_INITIALIZER("thesaurus"), SXML_AST_FIELD_THESAURUS},
    {SXML_BYTE_VIEW_INITIALIZER("times"), SXML_AST_FIELD_TIMES},
    {SXML_BYTE_VIEW_INITIALIZER("weight"), SXML_AST_FIELD_WEIGHT},
    {SXML_BYTE_VIEW_INITIALIZER("word"), SXML_AST_FIELD_WORD},
    {SXML_BYTE_VIEW_INITIALIZER("words"), SXML_AST_FIELD_WORDS},
};

/* A keyword token as written and the keyword detail it selects. */
typedef struct keyword_entry {
    sxml_byte_view token;
    sxml_ast_keyword keyword;
} keyword_entry;

/* The tokens of the any_all, unit, content and mode fields (3.2, 3.4.5,
   3.4.6, 3.5). */
static const keyword_entry full_text_keywords[] = {
    {SXML_BYTE_VIEW_INITIALIZER("any"), SXML_AST_KEYWORD_ANY},
    {SXML_BYTE_VIEW_INITIALIZER("all"), SXML_AST_KEYWORD_ALL},
    {SXML_BYTE_VIEW_INITIALIZER("phrase"), SXML_AST_KEYWORD_PHRASE},
    {SXML_BYTE_VIEW_INITIALIZER("words"), SXML_AST_KEYWORD_WORDS},
    {SXML_BYTE_VIEW_INITIALIZER("sentences"), SXML_AST_KEYWORD_SENTENCES},
    {SXML_BYTE_VIEW_INITIALIZER("paragraphs"), SXML_AST_KEYWORD_PARAGRAPHS},
    {SXML_BYTE_VIEW_INITIALIZER("sentence"), SXML_AST_KEYWORD_SENTENCE},
    {SXML_BYTE_VIEW_INITIALIZER("paragraph"), SXML_AST_KEYWORD_PARAGRAPH},
    {SXML_BYTE_VIEW_INITIALIZER("start"), SXML_AST_KEYWORD_START},
    {SXML_BYTE_VIEW_INITIALIZER("end"), SXML_AST_KEYWORD_END},
    {SXML_BYTE_VIEW_INITIALIZER("entire"), SXML_AST_KEYWORD_ENTIRE},
    {SXML_BYTE_VIEW_INITIALIZER("insensitive"), SXML_AST_KEYWORD_INSENSITIVE},
    {SXML_BYTE_VIEW_INITIALIZER("sensitive"), SXML_AST_KEYWORD_SENSITIVE},
    {SXML_BYTE_VIEW_INITIALIZER("lowercase"), SXML_AST_KEYWORD_LOWERCASE},
    {SXML_BYTE_VIEW_INITIALIZER("uppercase"), SXML_AST_KEYWORD_UPPERCASE},
};

/* A field whose presence sets one flag. */
typedef struct flag_entry {
    sxml_byte_view field_name;
    sxml_ast_flag flag;
} flag_entry;

static const flag_entry full_text_flags[] = {
    {SXML_BYTE_VIEW_INITIALIZER("separate_words"), SXML_AST_FLAG_SEPARATE_WORDS},
    {SXML_BYTE_VIEW_INITIALIZER("different"), SXML_AST_FLAG_DIFFERENT},
    {SXML_BYTE_VIEW_INITIALIZER("no"), SXML_AST_FLAG_NO},
    {SXML_BYTE_VIEW_INITIALIZER("uses_default"), SXML_AST_FLAG_DEFAULT},
};

/* ------------------------------------------------------------ Tokens */

static sxml_ast_keyword keyword_of(sxml_byte_view written) {
    for (size_t entry_index = 0;
         entry_index < sizeof full_text_keywords / sizeof full_text_keywords[0];
         entry_index += 1U) {
        if (sxml_byte_view_equals(written, full_text_keywords[entry_index].token)) {
            return full_text_keywords[entry_index].keyword;
        }
    }
    return SXML_AST_KEYWORD_NONE;
}

/*
 * The apply_token hook (D111): the tokens of Full Text's nodes become
 * detail and flags (src/xpath_ast_internal.h, sxml_ast_kind). Tokens of
 * other nodes, and those it does not claim, follow the part's and the
 * XPath rules.
 */
static bool apply_full_text_token(sxml_ast_parser_token token, sxml_ast_node_request *request) {
    const sxml_ast_kind kind = request->kind;
    if (kind < SXML_AST_KIND_FT_OPTION_DECLARATION || kind > SXML_AST_KIND_FT_EXTENSION_OPTION) {
        return false;
    }
    if (sxml_byte_view_equals(token.field_name, SXML_BYTE_VIEW_FROM_LITERAL("mode")) ||
        sxml_byte_view_equals(token.field_name, SXML_BYTE_VIEW_FROM_LITERAL("any_all")) ||
        sxml_byte_view_equals(token.field_name, SXML_BYTE_VIEW_FROM_LITERAL("unit")) ||
        (sxml_byte_view_equals(token.field_name, SXML_BYTE_VIEW_FROM_LITERAL("content")) &&
         kind == SXML_AST_KIND_FT_CONTENT)) {
        request->detail.keyword = keyword_of(token.written);
        return true;
    }
    for (size_t entry_index = 0; entry_index < sizeof full_text_flags / sizeof full_text_flags[0];
         entry_index += 1U) {
        if (sxml_byte_view_equals(token.field_name, full_text_flags[entry_index].field_name)) {
            request->flags |= (uint32_t)full_text_flags[entry_index].flag;
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------- Part */

const sxml_ast_parser_part sxml_full_text_parser_part = {
    .language = NULL,
    .root_node_type = {.bytes = NULL, .byte_count = 0},
    .node_types = {.entries = full_text_node_types,
                   .entry_count = sizeof full_text_node_types / sizeof full_text_node_types[0]},
    .fields = {.entries = full_text_fields,
               .entry_count = sizeof full_text_fields / sizeof full_text_fields[0]},
    .unnamed_children = {.entries = NULL, .entry_count = 0},
    .syntax_error_description = {.bytes = NULL, .byte_count = 0},
    .missing_token_description = {.bytes = NULL, .byte_count = 0},
    .apply_token = apply_full_text_token,
    .describe_payload = NULL,
    .finish_node = NULL,
    .extension = NULL,
};
