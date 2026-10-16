/*
 * Parse harness for tools/qt4-parse-check.py (docs/DECISIONS.md, D29).
 *
 * Reads queries from standard input, each as a line holding the byte count
 * followed by that many bytes, parses each with one tree-sitter grammar and
 * writes one result line per query:
 *   ok
 *   error <row>:<column> <kind>
 * With --tree, each result line is preceded by the syntax tree.
 * where <kind> is "ERROR", "MISSING <symbol>" or "delimitation" (two
 * non-delimiting terminals with no separator, XPath 4.0 A.3.2).
 *
 * Build: cc -O2 -DSXML_PARSE_LANGUAGE=tree_sitter_xpath -I third_party/tree-sitter/lib/include
 *        -I third_party/tree-sitter/lib/src third_party/tree-sitter/lib/src/lib.c
 *        grammars/xpath/src/parser.c grammars/xpath/src/scanner.c tools/qt4-parse-harness.c
 * This is a developer tool, not library code: it uses the C library freely.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tree_sitter/api.h"

const TSLanguage *SXML_PARSE_LANGUAGE(void);

typedef struct leaf_walk {
    const char *source_text;
    uint32_t previous_end_byte;
    bool previous_is_non_delimiting;
    bool has_previous;
    bool has_violation;
    TSPoint violation_point;
} leaf_walk;

/* XPath 4.0 A.3.2: names, keywords and numeric literals are non-delimiting. */
static bool is_non_delimiting(TSNode node)
{
    const char *type = ts_node_type(node);
    if (!ts_node_is_named(node)) {
        return isalpha((unsigned char)type[0]) != 0;
    }
    static const char *const named_types[] = {
        "ncname", "prefixed_name", "integer_literal", "decimal_literal", "double_literal",
        "hex_integer_literal", "binary_integer_literal",
    };
    for (size_t type_index = 0; type_index < sizeof named_types / sizeof named_types[0]; type_index += 1) {
        if (strcmp(type, named_types[type_index]) == 0) {
            return true;
        }
    }
    return false;
}

static void walk_leaves(TSNode node, leaf_walk *walk)
{
    uint32_t child_count = ts_node_child_count(node);
    if (child_count == 0) {
        bool non_delimiting = is_non_delimiting(node);
        uint32_t start_byte = ts_node_start_byte(node);
        if (walk->has_previous && walk->previous_is_non_delimiting && non_delimiting
            && walk->previous_end_byte == start_byte && !walk->has_violation) {
            walk->has_violation = true;
            walk->violation_point = ts_node_start_point(node);
        }
        walk->has_previous = true;
        walk->previous_is_non_delimiting = non_delimiting;
        walk->previous_end_byte = ts_node_end_byte(node);
        return;
    }
    for (uint32_t child_index = 0; child_index < child_count; child_index += 1) {
        walk_leaves(ts_node_child(node, child_index), walk);
    }
}

static bool find_error(TSNode node, TSNode *error_node)
{
    if (ts_node_is_error(node) || ts_node_is_missing(node)) {
        *error_node = node;
        return true;
    }
    if (!ts_node_has_error(node)) {
        return false;
    }
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t child_index = 0; child_index < child_count; child_index += 1) {
        if (find_error(ts_node_child(node, child_index), error_node)) {
            return true;
        }
    }
    *error_node = node;
    return true;
}

int main(int argument_count, char **arguments)
{
    bool prints_tree = argument_count > 1 && strcmp(arguments[1], "--tree") == 0;
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, SXML_PARSE_LANGUAGE());
    char header[64];
    while (fgets(header, sizeof header, stdin) != NULL) {
        size_t byte_count = (size_t)strtoul(header, NULL, 10);
        char *source_text = malloc(byte_count + 1);
        if (source_text == NULL || fread(source_text, 1, byte_count, stdin) != byte_count) {
            fprintf(stderr, "short read\n");
            return 1;
        }
        source_text[byte_count] = '\0';
        TSTree *tree = ts_parser_parse_string(parser, NULL, source_text, (uint32_t)byte_count);
        TSNode root = ts_tree_root_node(tree);
        if (prints_tree) {
            char *tree_text = ts_node_string(root);
            printf("%s\n", tree_text);
            free(tree_text);
        }
        TSNode error_node;
        if (find_error(root, &error_node)) {
            TSPoint point = ts_node_start_point(error_node);
            if (ts_node_is_missing(error_node)) {
                printf("error %u:%u MISSING %s\n", point.row + 1, point.column + 1, ts_node_type(error_node));
            } else {
                printf("error %u:%u ERROR\n", point.row + 1, point.column + 1);
            }
        } else {
            leaf_walk walk = {.source_text = source_text};
            walk_leaves(root, &walk);
            if (walk.has_violation) {
                printf("error %u:%u delimitation\n", walk.violation_point.row + 1,
                       walk.violation_point.column + 1);
            } else {
                printf("ok\n");
            }
        }
        fflush(stdout);
        ts_tree_delete(tree);
        free(source_text);
    }
    ts_parser_delete(parser);
    return 0;
}
