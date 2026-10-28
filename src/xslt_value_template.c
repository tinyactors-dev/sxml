/*
 * Value templates (XSLT 4.0, section 5.6): text to the XPath AST of a
 * string template (src/xslt_value_template_internal.h; docs/DECISIONS.md
 * D97, D102, D111).
 *
 * Golden file: src/xpath_parse.c (the parser part seam and its tables)
 * and the parts of tests/xpath_ast_test.c.
 *
 * The grammar grammars/value_template inherits grammars/xpath and adds
 * three node types: value_template (the XPATH root), value_template_body
 * (its STRING_TEMPLATE body; a zero-width token keeps it present for a
 * zero-length template) and value_template_fixed_part. The XPath walker
 * builds the AST; this part's describe_payload hook decodes the fixed
 * parts of the value template itself, whose escapes differ from those of
 * an XPath string template nested in a variable part.
 *
 * A syntax error is XPST0003 from the walker. Which brace rule failed is
 * then found by a lexical scan of the text (fixed parts, string literals,
 * comments, nested curly brackets and string templates), so that an
 * unescaped "}" is XTSE0370 and an unclosed "{" XTSE0350 (5.6). The scan
 * keeps its nesting in one 64-bit word, so it needs no memory; a deeper
 * template keeps XPST0003.
 */
#include "xslt_value_template_internal.h"

#include "sxml/string.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defined by the generated parser of grammars/value_template (D12). */
const struct TSLanguage *
tree_sitter_value_template(void); // NOLINT(readability-identifier-naming)

/* --------------------------------------------------------- Parser part */

/* Sorted by node type in byte order (sxml_ast_node_type_view). */
static const sxml_ast_node_type_entry sxml_value_template_node_types[] = {
    {SXML_BYTE_VIEW_INITIALIZER("value_template"), SXML_AST_KIND_XPATH},
    {SXML_BYTE_VIEW_INITIALIZER("value_template_body"),
     SXML_AST_KIND_STRING_TEMPLATE},
    {SXML_BYTE_VIEW_INITIALIZER("value_template_fixed_part"),
     SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART},
};

/* The node made first, the XPATH root (index 0 is "no node"). */
static const sxml_ast_node_index sxml_value_template_root = {.value = 1};

/*
 * True when a fixed part starting at byte_offset belongs to the value
 * template itself, not to a string template in one of its variable parts.
 * The walk adds nodes in source order and appends each to its parent at
 * once, so a variable part being walked is the body's last child.
 */
static bool sxml_value_template_is_own_fixed_part(const sxml_ast *ast,
                                                  uint32_t byte_offset) {
    const sxml_ast_node *body = sxml_ast_get_node(
        ast, sxml_ast_get_node(ast, sxml_value_template_root)->first_child);
    if (body->kind != SXML_AST_KIND_STRING_TEMPLATE) {
        return false;
    }
    const sxml_ast_node *last_part = sxml_ast_get_node(ast, body->last_child);
    if (last_part->kind != SXML_AST_KIND_ENCLOSED_EXPRESSION) {
        return true;
    }
    const sxml_ast_source_span span = last_part->source_span;
    return byte_offset < span.byte_offset ||
           byte_offset - span.byte_offset >= span.byte_count;
}

/*
 * Stores the expansion of a fixed part (5.6): "{{" and "}}" become single
 * curly brackets; nothing else is an escape (a backquote is text here).
 */
static sxml_status sxml_value_template_store_fixed_part(sxml_ast *ast,
                                                        sxml_byte_view written,
                                                        sxml_ast_text *text) {
    size_t piece_start = 0;
    size_t position = 0;
    while (position < written.byte_count) {
        const uint8_t byte = written.bytes[position];
        const bool is_doubled = (byte == '{' || byte == '}') &&
                                position + 1U < written.byte_count &&
                                written.bytes[position + 1U] == byte;
        if (!is_doubled) {
            position += 1U;
            continue;
        }
        /* The piece up to and including the first bracket of the pair. */
        const sxml_status status = sxml_ast_append_text(
            ast,
            (sxml_byte_view){.bytes = written.bytes + piece_start,
                             .byte_count = position + 1U - piece_start},
            text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        position += 2U;
        piece_start = position;
    }
    if (piece_start == written.byte_count) {
        return SXML_STATUS_OK;
    }
    return sxml_ast_append_text(
        ast,
        (sxml_byte_view){.bytes = written.bytes + piece_start,
                         .byte_count = written.byte_count - piece_start},
        text);
}

/* describe_payload (D111): the fixed parts of the value template. */
static sxml_status
sxml_value_template_describe_payload(sxml_ast_parser_node node,
                                     sxml_ast_node_request *request,
                                     sxml_ast_payload_outcome *outcome) {
    if (request->kind != SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
        return SXML_STATUS_OK;
    }
    sxml_ast *ast = sxml_ast_front_end_get_ast(node.front_end);
    if (!sxml_value_template_is_own_fixed_part(
            ast, request->source_span.byte_offset)) {
        return SXML_STATUS_OK;
    }
    sxml_ast_text text = {.byte_offset = 0, .byte_count = 0};
    const sxml_status status =
        sxml_value_template_store_fixed_part(ast, node.written, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    request->payload.text = text;
    outcome->is_described = true;
    return SXML_STATUS_OK;
}

static const sxml_ast_parser_part sxml_value_template_part = {
    .language = tree_sitter_value_template,
    .root_node_type = SXML_BYTE_VIEW_INITIALIZER("value_template"),
    .node_types = {.entries = sxml_value_template_node_types,
                   .entry_count = sizeof sxml_value_template_node_types /
                                  sizeof sxml_value_template_node_types[0]},
    /* body and part are fields of grammars/xpath already. */
    .fields = {.entries = NULL, .entry_count = 0},
    .unnamed_children = {.entries = NULL, .entry_count = 0},
    .syntax_error_description = SXML_BYTE_VIEW_INITIALIZER(
        "a variable part of this value template is not an XPath expression"),
    .missing_token_description = SXML_BYTE_VIEW_INITIALIZER(
        "a variable part of this value template is incomplete"),
    .apply_token = NULL,
    .describe_payload = sxml_value_template_describe_payload,
    .finish_node = NULL,
};

/* ------------------------------------------------- Curly bracket errors */

/* What the lexical scan found. */
typedef struct sxml_value_template_brace_error {
    /* XTSE0350 or XTSE0370; empty when the brackets match. */
    sxml_byte_view error_code;
    sxml_byte_view description;
    /* The offending curly bracket, as an offset into the text. */
    size_t byte_offset;
} sxml_value_template_brace_error;

/* Nesting the scan follows; deeper templates keep XPST0003. */
enum { SXML_VALUE_TEMPLATE_MAXIMUM_NESTING = 64 };

/* Where the scan is. Levels are curly brackets or string templates. */
typedef struct sxml_value_template_scan {
    sxml_byte_view text;
    size_t position;
    /* Open levels; 0 is a fixed part of the value template. */
    uint32_t level_count;
    /* Bit n set: level n + 1 is a string template, else a "{". */
    uint64_t string_template_levels;
    /* The "{" that opened level 1. */
    size_t variable_part_offset;
    /* Inside a string literal, its delimiter; else 0. */
    uint8_t string_delimiter;
    /* Nesting of "(: ... :)" comments (XPath 4.0, A.3.1). */
    uint32_t comment_depth;
    /* Set when the text breaks XPath rules the scan does not judge, or
       nests too deep: no curly bracket error is reported. */
    bool is_abandoned;
} sxml_value_template_scan;

static bool sxml_value_template_next_is(const sxml_value_template_scan *scan,
                                        uint8_t byte) {
    return scan->position + 1U < scan->text.byte_count &&
           scan->text.bytes[scan->position + 1U] == byte;
}

/* What an open level of the scan is. */
typedef enum sxml_value_template_level_kind {
    /* "{": a variable part, or an XPath construct in one. */
    SXML_VALUE_TEMPLATE_LEVEL_KIND_CURLY_BRACKET,
    /* "`": an XPath string template in a variable part. */
    SXML_VALUE_TEMPLATE_LEVEL_KIND_STRING_TEMPLATE,
} sxml_value_template_level_kind;

static void sxml_value_template_open(sxml_value_template_scan *scan,
                                     sxml_value_template_level_kind kind) {
    if (scan->level_count == SXML_VALUE_TEMPLATE_MAXIMUM_NESTING) {
        scan->is_abandoned = true;
        return;
    }
    if (scan->level_count == 0) {
        scan->variable_part_offset = scan->position;
    }
    const uint64_t bit = (uint64_t)1U << scan->level_count;
    if (kind == SXML_VALUE_TEMPLATE_LEVEL_KIND_STRING_TEMPLATE) {
        scan->string_template_levels |= bit;
    } else {
        scan->string_template_levels &= ~bit;
    }
    scan->level_count += 1U;
    scan->position += 1U;
}

/* True when the innermost open level is a string template. */
static bool sxml_value_template_in_string_template(
    const sxml_value_template_scan *scan) {
    const uint64_t bit = (uint64_t)1U << (scan->level_count - 1U);
    return (scan->string_template_levels & bit) != 0;
}

/* One step in a variable part: XPath tokens that hold curly brackets. */
static void
sxml_value_template_step_expression(sxml_value_template_scan *scan) {
    const uint8_t byte = scan->text.bytes[scan->position];
    if (scan->comment_depth != 0) {
        if (byte == '(' && sxml_value_template_next_is(scan, ':')) {
            scan->comment_depth += 1U;
            scan->position += 2U;
        } else if (byte == ':' && sxml_value_template_next_is(scan, ')')) {
            scan->comment_depth -= 1U;
            scan->position += 2U;
        } else {
            scan->position += 1U;
        }
        return;
    }
    if (scan->string_delimiter != 0) {
        /* A doubled delimiter is an escape (XPath 4.0, A.2.1
           StringLiteral); it reads as a close and a reopen. */
        if (byte == scan->string_delimiter) {
            scan->string_delimiter = 0;
        }
        scan->position += 1U;
        return;
    }
    switch (byte) {
    case '\'':
    case '"':
        scan->string_delimiter = byte;
        scan->position += 1U;
        return;
    case '(':
        if (sxml_value_template_next_is(scan, ':')) {
            scan->comment_depth = 1;
            scan->position += 2U;
            return;
        }
        scan->position += 1U;
        return;
    case '{':
        sxml_value_template_open(
            scan, SXML_VALUE_TEMPLATE_LEVEL_KIND_CURLY_BRACKET);
        return;
    case '`':
        sxml_value_template_open(
            scan, SXML_VALUE_TEMPLATE_LEVEL_KIND_STRING_TEMPLATE);
        return;
    case '}':
        scan->level_count -= 1U;
        scan->position += 1U;
        return;
    default:
        scan->position += 1U;
        return;
    }
}

/* One step in a string template (XPath 4.0, 4.9.3). */
static void
sxml_value_template_step_string_template(sxml_value_template_scan *scan) {
    const uint8_t byte = scan->text.bytes[scan->position];
    if ((byte == '{' || byte == '}' || byte == '`') &&
        sxml_value_template_next_is(scan, byte)) {
        scan->position += 2U;
        return;
    }
    if (byte == '{') {
        sxml_value_template_open(
            scan, SXML_VALUE_TEMPLATE_LEVEL_KIND_CURLY_BRACKET);
    } else if (byte == '`') {
        scan->level_count -= 1U;
        scan->position += 1U;
    } else if (byte == '}') {
        scan->is_abandoned = true;
    } else {
        scan->position += 1U;
    }
}

/*
 * Finds the curly bracket that breaks the rules of 5.6 in text, if any:
 * XTSE0370 for an unescaped "}" in a fixed part, XTSE0350 for a variable
 * part that never closes. Loops; no recursion.
 */
static sxml_value_template_brace_error
sxml_value_template_find_brace_error(sxml_byte_view text) {
    sxml_value_template_brace_error found = {
        .error_code = {.bytes = NULL, .byte_count = 0},
        .description = {.bytes = NULL, .byte_count = 0},
        .byte_offset = 0};
    sxml_value_template_scan scan = {.text = text,
                                     .position = 0,
                                     .level_count = 0,
                                     .string_template_levels = 0,
                                     .variable_part_offset = 0,
                                     .string_delimiter = 0,
                                     .comment_depth = 0,
                                     .is_abandoned = false};
    while (scan.position < text.byte_count && !scan.is_abandoned) {
        if (scan.level_count != 0) {
            if (sxml_value_template_in_string_template(&scan)) {
                sxml_value_template_step_string_template(&scan);
            } else {
                sxml_value_template_step_expression(&scan);
            }
            continue;
        }
        const uint8_t byte = text.bytes[scan.position];
        if ((byte == '{' || byte == '}') &&
            sxml_value_template_next_is(&scan, byte)) {
            scan.position += 2U;
        } else if (byte == '{') {
            sxml_value_template_open(
                &scan, SXML_VALUE_TEMPLATE_LEVEL_KIND_CURLY_BRACKET);
        } else if (byte == '}') {
            /* XSLT 4.0, 5.6: [ERR XTSE0370]. */
            found.error_code = SXML_BYTE_VIEW_FROM_LITERAL("XTSE0370");
            found.description = SXML_BYTE_VIEW_FROM_LITERAL(
                "a right curly bracket in a fixed part of a value template "
                "must be written as }}");
            found.byte_offset = scan.position;
            return found;
        } else {
            scan.position += 1U;
        }
    }
    if (!scan.is_abandoned && scan.level_count != 0) {
        /* XSLT 4.0, 5.6: [ERR XTSE0350]. */
        found.error_code = SXML_BYTE_VIEW_FROM_LITERAL("XTSE0350");
        found.description = SXML_BYTE_VIEW_FROM_LITERAL(
            "a left curly bracket in a value template has no matching right "
            "curly bracket; a literal one is written as {{");
        found.byte_offset = scan.variable_part_offset;
    }
    return found;
}

/* The error record's location of byte_offset: lines and codepoint
   columns from 1, as the XPath front end counts them. */
static sxml_source_location
sxml_value_template_location_of(const sxml_ast_build_request *request,
                                size_t byte_offset) {
    sxml_source_location location = {.source_name = request->source_name,
                                     .line_number = 1,
                                     .column_number = 1,
                                     .byte_offset = byte_offset};
    const sxml_byte_view text = request->expression_text;
    for (size_t position = 0; position < byte_offset; position += 1U) {
        const uint8_t byte = text.bytes[position];
        if (byte == '\n') {
            location.line_number += 1U;
            location.column_number = 1;
        } else if ((byte & 0xC0U) != 0x80U) {
            location.column_number += 1U;
        }
    }
    return location;
}

/* ---------------------------------------------------------- Plain text */

/*
 * True when text is a value template of fixed text only (XSLT 4.0, 5.6.1:
 * no curly bracket, so no variable part and no escape): valid UTF-8 whose
 * every character is one the fixed part scanner accepts (XML 1.1 [2]
 * Char; grammars/value_template/src/scanner.c), so the parse cannot fail.
 * Any other text goes to the parser, which judges it.
 */
static bool sxml_value_template_is_plain_text(sxml_byte_view text) {
    if ((text.bytes == NULL && text.byte_count != 0) ||
        text.byte_count > UINT32_MAX) {
        return false;
    }
    for (size_t position = 0; position < text.byte_count; position += 1U) {
        const uint8_t byte = text.bytes[position];
        /* NUL, the brackets, and the lead byte of U+FFFE and U+FFFF
           (EF BF BE, EF BF BF), the non-characters UTF-8 allows. */
        if (byte == 0 || byte == '{' || byte == '}' ||
            (byte == 0xEFU && position + 2U < text.byte_count &&
             text.bytes[position + 1U] == 0xBFU &&
             (text.bytes[position + 2U] == 0xBEU ||
              text.bytes[position + 2U] == 0xBFU))) {
            return false;
        }
    }
    sxml_utf8_validation validation = {
        .valid_byte_count = 0, .codepoint_count = 0, .is_ascii = false};
    return sxml_utf8_validate(text, &validation) == SXML_STATUS_OK;
}

/*
 * The AST the parser builds for plain text, without the parse (perf-13):
 * the XPATH root, its STRING_TEMPLATE body and, unless the text is empty,
 * one fixed part holding the text; every span the whole text from line 1,
 * column 1. The capacities are the parser's: one node per concrete syntax
 * tree node, as many text bytes as the text has.
 */
static sxml_status
sxml_value_template_build_plain(const sxml_ast_build_request *request,
                                sxml_ast **ast) {
    const sxml_byte_view text = request->expression_text;
    const sxml_ast_source_span span = {.byte_offset = 0,
                                       .byte_count = (uint32_t)text.byte_count,
                                       .line_number = 1,
                                       .column_number = 1};
    sxml_ast *new_ast = NULL;
    sxml_status status = sxml_ast_create(
        request->allocator,
        &(sxml_ast_configuration){.node_capacity =
                                      text.byte_count == 0 ? 2U : 3U,
                                  .text_byte_capacity = text.byte_count,
                                  .source_name = request->source_name},
        &new_ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ast_node_index root = {.value = 0};
    sxml_ast_node_index body = {.value = 0};
    status = sxml_ast_add_node(new_ast,
                               &(sxml_ast_node_request){
                                   .kind = SXML_AST_KIND_XPATH,
                                   .detail = {.operator = SXML_AST_OPERATOR_NONE},
                                   .flags = 0,
                                   .source_span = span,
                                   .payload = {.text = {.byte_offset = 0,
                                                        .byte_count = 0}}},
                               &root);
    if (status == SXML_STATUS_OK) {
        status = sxml_ast_add_node(
            new_ast,
            &(sxml_ast_node_request){
                .kind = SXML_AST_KIND_STRING_TEMPLATE,
                .detail = {.operator = SXML_AST_OPERATOR_NONE},
                .flags = 0,
                .source_span = span,
                .payload = {.text = {.byte_offset = 0, .byte_count = 0}}},
            &body);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ast_append_child(
            new_ast, (sxml_ast_child_request){.parent = root,
                                              .field = SXML_AST_FIELD_BODY,
                                              .child = body});
    }
    if (status == SXML_STATUS_OK && text.byte_count != 0) {
        sxml_ast_text stored = {.byte_offset = 0, .byte_count = 0};
        sxml_ast_node_index fixed_part = {.value = 0};
        status = sxml_ast_append_text(new_ast, text, &stored);
        if (status == SXML_STATUS_OK) {
            status = sxml_ast_add_node(
                new_ast,
                &(sxml_ast_node_request){
                    .kind = SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART,
                    .detail = {.operator = SXML_AST_OPERATOR_NONE},
                    .flags = 0,
                    .source_span = span,
                    .payload = {.text = stored}},
                &fixed_part);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_ast_append_child(
                new_ast, (sxml_ast_child_request){.parent = body,
                                                  .field = SXML_AST_FIELD_PART,
                                                  .child = fixed_part});
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_ast_destroy(new_ast);
        return status;
    }
    new_ast->root = root;
    *ast = new_ast;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Entry */

sxml_status
sxml_xslt_build_value_template_ast(const sxml_ast_build_request *request,
                                   sxml_ast **ast) {
    if (request == NULL || ast == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* XSLT 4.0, 5.6.1: without curly brackets a value template is its
       text, so there is nothing to parse. The parser's depth limit counts
       the three nodes, so a smaller limit goes to the parser. */
    if ((request->maximum_expression_depth == 0 ||
         request->maximum_expression_depth >= 3U) &&
        sxml_value_template_is_plain_text(request->expression_text)) {
        return sxml_value_template_build_plain(request, ast);
    }
    const sxml_status status = sxml_ast_build_with_parser_part(
        request, &sxml_value_template_part, ast);
    if (status != SXML_STATUS_STATIC_ERROR || request->error_record == NULL) {
        return status;
    }
    const sxml_value_template_brace_error brace_error =
        sxml_value_template_find_brace_error(request->expression_text);
    if (brace_error.error_code.byte_count != 0) {
        sxml_error_record *error_record = request->error_record;
        error_record->code.local_name = brace_error.error_code;
        error_record->diagnostic.description = brace_error.description;
        error_record->diagnostic.location =
            sxml_value_template_location_of(request, brace_error.byte_offset);
    }
    return status;
}
