/*
 * The Full Text parts of XQuery's static analysis and lowering that only
 * the optional feature full-text links (src/full_text_internal.h; D189,
 * D214): the static rules and the lowering of "E contains text S" to a
 * match program. src/xquery_lower_full_text.c, which XQuery's lowering
 * always links, reaches them through the feature's operations, so a
 * program without the feature (sxml.wasm) does not carry them.
 *
 * Lowering. `$p contains text "a" ftand {$w} window 3 words` becomes
 *
 *     (let $1 (coerce $w xs:string* function-argument)
 *       (let $2 (array-constructor square
 *                  "W2,0,0,0,0,0,0,0,0;W3,0,0,0,0,0,0,0,0;A;I4,0;" "a" $1 3)
 *         (static-call contains $p $2 ())))
 *
 * A post-order walk of the selection by its links (D71) gives the
 * instructions in program order; each node's operand expressions are
 * lowered one per step, coerced as Full Text 3.0, 4.3 step 1 says, and
 * become members of the program array. The effective match options of
 * each FTWords (3.4: the innermost FTMatchOptions first, then the
 * enclosing ones, the prolog's ft-option declarations, the defaults) are
 * static, so they are written into its instruction; a static stop word
 * list becomes a constant member.
 *
 * Golden file: src/xquery_lower_update.c (internal rows called through
 * STATIC_CALL, procedures whose steps ask the machine for their parts).
 *
 * Section numbers are of XQuery and XPath Full Text 3.0 (W3C
 * Recommendation, 24 November 2015).
 */
#include "full_text_internal.h"

#include "ir_internal.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"
#include "xpath_lower_internal.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

/* ========================================================= AST access */

static const sxml_ast_node *node_in(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_get_node(ast, node);
}

static sxml_ast_node_index child_in(const sxml_ast *ast, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_in(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(ast, node);
}

/* Whether kind is one of Full Text's (D214). */
static bool is_full_text_kind(sxml_ast_kind kind) {
    return kind >= SXML_AST_KIND_FT_OPTION_DECLARATION && kind <= SXML_AST_KIND_FT_EXTENSION_OPTION;
}

/* Whether kind is a selection or positional filter kind: the nodes the
   program has instructions for. */
static bool is_program_kind(sxml_ast_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_FT_SELECTION:
    case SXML_AST_KIND_FT_OR:
    case SXML_AST_KIND_FT_AND:
    case SXML_AST_KIND_FT_MILD_NOT:
    case SXML_AST_KIND_FT_UNARY_NOT:
    case SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS:
    case SXML_AST_KIND_FT_WORDS:
    case SXML_AST_KIND_FT_EXTENSION_SELECTION:
    case SXML_AST_KIND_FT_ORDER:
    case SXML_AST_KIND_FT_WINDOW:
    case SXML_AST_KIND_FT_DISTANCE:
    case SXML_AST_KIND_FT_SCOPE:
    case SXML_AST_KIND_FT_CONTENT:
        return true;
    default:
        return false;
    }
}

/* ======================================================= Static rules */

/* The match option groups of 3.4 (FTST0019: at most one of each in one
   FTMatchOptions); extension options may repeat. */
static size_t option_group(sxml_ast_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_FT_LANGUAGE_OPTION:
        return 1;
    case SXML_AST_KIND_FT_WILDCARD_OPTION:
        return 2;
    case SXML_AST_KIND_FT_THESAURUS_OPTION:
        return 3;
    case SXML_AST_KIND_FT_STEM_OPTION:
        return 4;
    case SXML_AST_KIND_FT_CASE_OPTION:
        return 5;
    case SXML_AST_KIND_FT_DIACRITICS_OPTION:
        return 6;
    case SXML_AST_KIND_FT_STOP_WORD_OPTION:
        return 7;
    default:
        return 0;
    }
}

#define SXML_FULL_TEXT_OPTION_GROUP_COUNT 8U

/* What the rules walk over one module works with. */
typedef struct rules_walk {
    const sxml_xquery_full_text_rules_request *request;
    const sxml_ast *ast;
} rules_walk;

static sxml_status report_at(const rules_walk *walk, sxml_ast_node_index node,
                             sxml_byte_view code_and_description) {
    return sxml_xpath_report_static_error(
        &(sxml_xpath_static_error){
            .ast = walk->ast,
            .node = node,
            .code_local_name = {.bytes = code_and_description.bytes, .byte_count = 8},
            .description = {.bytes = code_and_description.bytes + 9,
                            .byte_count = code_and_description.byte_count - 9}},
        walk->request->error_record);
}

/* FTST0019 for an FTMatchOptions with two options of one group. */
static sxml_status check_option_groups(const rules_walk *walk, sxml_ast_node_index owner) {
    bool is_seen[SXML_FULL_TEXT_OPTION_GROUP_COUNT] = {false};
    for (sxml_ast_node_index option = child_in(walk->ast, owner, SXML_AST_FIELD_OPTION);
         option.value != 0; option = next_in(walk->ast, option)) {
        const size_t group = option_group(node_in(walk->ast, option)->kind);
        if (group == 0) {
            continue;
        }
        if (is_seen[group]) {
            return report_at(walk, option,
                             SXML_BYTE_VIEW_FROM_LITERAL(
                                 "FTST0019 one FTMatchOptions has two match options of one "
                                 "group (Full Text 3.0, 3.4)"));
        }
        is_seen[group] = true;
    }
    return SXML_STATUS_OK;
}

/* Whether two variable name nodes are the same name as written (the
   prefixes of one module bind one URI each). */
static bool are_same_names(const sxml_ast *ast, sxml_ast_node_index first,
                           sxml_ast_node_index second) {
    if (first.value == 0 || second.value == 0) {
        return false;
    }
    const sxml_ast_name first_name = node_in(ast, first)->payload.name;
    const sxml_ast_name second_name = node_in(ast, second)->payload.name;
    return sxml_byte_view_equals(sxml_ast_get_text(ast, first_name.local_name),
                                 sxml_ast_get_text(ast, second_name.local_name)) &&
           sxml_byte_view_equals(sxml_ast_get_text(ast, first_name.namespace_prefix),
                                 sxml_ast_get_text(ast, second_name.namespace_prefix)) &&
           sxml_byte_view_equals(sxml_ast_get_text(ast, first_name.namespace_uri),
                                 sxml_ast_get_text(ast, second_name.namespace_uri));
}

/* XQST0089: a score variable named as its for variable or positional
   variable (2.3.1). */
static sxml_status check_score_variable(const rules_walk *walk, sxml_ast_node_index score) {
    const sxml_ast *ast = walk->ast;
    const sxml_ast_node_index binding = node_in(ast, score)->parent;
    const sxml_ast_node_index name = child_in(ast, score, SXML_AST_FIELD_NAME);
    const sxml_ast_node_index variable = child_in(
        ast, child_in(ast, binding, SXML_AST_FIELD_VARIABLE), SXML_AST_FIELD_NAME);
    const sxml_ast_node_index position =
        child_in(ast, child_in(ast, binding, SXML_AST_FIELD_POSITION), SXML_AST_FIELD_NAME);
    if (are_same_names(ast, name, variable) || are_same_names(ast, name, position)) {
        return report_at(walk, score,
                         SXML_BYTE_VIEW_FROM_LITERAL(
                             "XQST0089 the score variable has the name of its for or "
                             "positional variable (Full Text 3.0, 2.3.1)"));
    }
    return SXML_STATUS_OK;
}

/* The languages sxml supports (3.4.1: implementation-defined; FTST0009
   for others): its matching is the same for each, and none needs word
   segmentation, which Chinese, Japanese and Thai would (D214). */
static const sxml_byte_view supported_languages[] = {
    SXML_BYTE_VIEW_INITIALIZER("en"), SXML_BYTE_VIEW_INITIALIZER("de"),
    SXML_BYTE_VIEW_INITIALIZER("fr"), SXML_BYTE_VIEW_INITIALIZER("it"),
    SXML_BYTE_VIEW_INITIALIZER("es"), SXML_BYTE_VIEW_INITIALIZER("nl"),
};

/* FTST0009 unless the language's primary subtag is one sxml supports. */
static sxml_status check_language(const rules_walk *walk, sxml_ast_node_index option) {
    const sxml_ast_node_index literal = child_in(walk->ast, option, SXML_AST_FIELD_LANGUAGE);
    const sxml_byte_view language =
        sxml_ast_get_text(walk->ast, node_in(walk->ast, literal)->payload.text);
    size_t subtag_length = 0;
    while (subtag_length < language.byte_count && language.bytes[subtag_length] != '-') {
        subtag_length += 1;
    }
    for (size_t language_index = 0;
         language_index < sizeof supported_languages / sizeof supported_languages[0];
         language_index += 1) {
        const sxml_byte_view supported = supported_languages[language_index];
        bool is_same = subtag_length == supported.byte_count;
        for (size_t byte_index = 0; is_same && byte_index < subtag_length; byte_index += 1) {
            const uint8_t byte = language.bytes[byte_index];
            const uint8_t lower = byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + ('a' - 'A')) : byte;
            is_same = lower == supported.bytes[byte_index];
        }
        if (is_same) {
            return SXML_STATUS_OK;
        }
    }
    return report_at(walk, option,
                     SXML_BYTE_VIEW_FROM_LITERAL(
                         "FTST0009 sxml does not support the language of the language option "
                         "(Full Text 3.0, 3.4.1)"));
}

/* The rules for one Full Text node. */
static sxml_status check_node(const rules_walk *walk, sxml_ast_node_index node) {
    const sxml_ast_node *ast_node = node_in(walk->ast, node);
    switch ((uint32_t)ast_node->kind) {
    case SXML_AST_KIND_FT_OPTION_DECLARATION:
    case SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS:
        return check_option_groups(walk, node);
    case SXML_AST_KIND_FT_SCORE_VARIABLE:
        return check_score_variable(walk, node);
    case SXML_AST_KIND_FT_STOP_WORDS:
        /* 3.4.7 and 5.2.11: sxml restricts stop words to explicit lists
           until it reads lists by URI (packet 7.12c). */
        if (child_in(walk->ast, node, SXML_AST_FIELD_URI).value != 0) {
            return report_at(walk, node,
                             SXML_BYTE_VIEW_FROM_LITERAL(
                                 "FTST0006 sxml supports stop word lists given as strings, not "
                                 "by URI (Full Text 3.0, 3.4.7)"));
        }
        return SXML_STATUS_OK;
    case SXML_AST_KIND_FT_LANGUAGE_OPTION:
        return check_language(walk, node);
    case SXML_AST_KIND_FT_THESAURUS_ID:
        /* 3.4.3: nor thesauri by URI (7.12c). */
        return report_at(walk, node,
                         SXML_BYTE_VIEW_FROM_LITERAL(
                             "FTST0018 the thesaurus is not among the statically known "
                             "thesauri (Full Text 3.0, 3.4.3)"));
    case SXML_AST_KIND_FT_EXTENSION_SELECTION:
        /* 3.6: sxml recognizes no pragma, so the selection is required. */
        if (child_in(walk->ast, node, SXML_AST_FIELD_SELECTION).value == 0) {
            return report_at(walk, node,
                             SXML_BYTE_VIEW_FROM_LITERAL(
                                 "XQST0079 an extension selection has no selection and no "
                                 "pragma sxml recognizes (Full Text 3.0, 3.6)"));
        }
        return SXML_STATUS_OK;
    default:
        return SXML_STATUS_OK;
    }
}

sxml_status sxml_full_text_check_rules(const sxml_xquery_full_text_rules_request *request) {
    const sxml_ast *ast = request->ast;
    const rules_walk walk = {.request = request, .ast = ast};
    sxml_status status = SXML_STATUS_OK;
    /* A pre-order walk of every node by the links: no stack (D71). */
    sxml_ast_node_index node = ast->root;
    while (status == SXML_STATUS_OK && node.value != 0) {
        const sxml_ast_node *current = node_in(ast, node);
        if (is_full_text_kind(current->kind)) {
            status = check_node(&walk, node);
        }
        if (current->first_child.value != 0) {
            node = current->first_child;
            continue;
        }
        while (node.value != 0 && node_in(ast, node)->next_sibling.value == 0) {
            node = node_in(ast, node)->parent;
        }
        if (node.value != 0) {
            node = node_in(ast, node)->next_sibling;
        }
    }
    return status;
}

/* ========================================================= Lowering */

static const sxml_ast_node *node_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_get_node(state->ast, node);
}

static sxml_ast_node_index child_of(const sxml_xpath_lowerer *state, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return child_in(state->ast, parent, field);
}

/* An expression bound in block. */
typedef struct expression_in_block {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_expression expression;
} expression_in_block;

static sxml_status bind_new_expression(sxml_xpath_lowerer *state, expression_in_block request,
                                       sxml_ir_atom *atom) {
    sxml_ir_expression_index index = {0};
    const sxml_status status = sxml_xpath_lowerer_add_expression(
        state,
        (sxml_xpath_lowering_expression_to_add){.node = request.node,
                                                .expression = request.expression},
        &index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = request.block, .expression = index},
        atom);
}

/* A STATIC_CALL of an internal row. */
typedef struct native_call {
    sxml_ast_node_index node;
    const sxml_native_function *native_function;
    sxml_ir_atom_view arguments;
} native_call;

static sxml_status make_native_call(sxml_xpath_lowerer *state, native_call request,
                                    sxml_ir_expression *expression) {
    sxml_ir_atom_list arguments = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(state->unit, request.arguments, &arguments);
    if (status == SXML_STATUS_OK) {
        *expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
            .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                       .native_function = request.native_function},
                            .arguments = arguments}};
    }
    return status;
}

/* ------------------------------------------------ Match options (3.4) */

/* The effective options of one FTWords, as the instruction writes them. */
typedef struct effective_options {
    sxml_full_text_match_options options;
    /* The stop word option that applies; 0 for "no stop words". */
    sxml_ast_node_index stop_words;
} effective_options;

/* Applies the options of owner (an FT_PRIMARY_WITH_OPTIONS or
   FT_OPTION_DECLARATION) to effective, each group at most once per
   `settled` (the groups an inner FTMatchOptions already set stay). */
typedef struct option_source {
    sxml_ast_node_index owner;
    bool *settled;
} option_source;

static void take_options(const sxml_xpath_lowerer *state, option_source source,
                         effective_options *effective) {
    for (sxml_ast_node_index option = child_of(state, source.owner, SXML_AST_FIELD_OPTION);
         option.value != 0; option = next_in(state->ast, option)) {
        const sxml_ast_node *node = node_of(state, option);
        const size_t group = option_group(node->kind);
        if (group == 0 || source.settled[group]) {
            continue;
        }
        source.settled[group] = true;
        const bool is_no = (node->flags & SXML_AST_FLAG_NO) != 0;
        switch ((uint32_t)node->kind) {
        case SXML_AST_KIND_FT_WILDCARD_OPTION:
            effective->options.uses_wildcards = !is_no;
            break;
        case SXML_AST_KIND_FT_STEM_OPTION:
            effective->options.uses_stemming = !is_no;
            break;
        case SXML_AST_KIND_FT_CASE_OPTION:
            effective->options.case_mode =
                node->detail.keyword == SXML_AST_KEYWORD_SENSITIVE ? SXML_FULL_TEXT_CASE_SENSITIVE
                : node->detail.keyword == SXML_AST_KEYWORD_LOWERCASE
                    ? SXML_FULL_TEXT_CASE_LOWERCASE
                : node->detail.keyword == SXML_AST_KEYWORD_UPPERCASE
                    ? SXML_FULL_TEXT_CASE_UPPERCASE
                    : SXML_FULL_TEXT_CASE_INSENSITIVE;
            break;
        case SXML_AST_KIND_FT_DIACRITICS_OPTION:
            effective->options.is_diacritics_sensitive =
                node->detail.keyword == SXML_AST_KEYWORD_SENSITIVE;
            break;
        case SXML_AST_KIND_FT_STOP_WORD_OPTION:
            effective->stop_words = is_no ? (sxml_ast_node_index){0} : option;
            break;
        default:
            /* Language and thesaurus: sxml's matching does not depend on
               the language, and the default thesaurus is empty. */
            break;
        }
    }
}

/* The options of words (3.4, 2.4): the defaults (case insensitive,
   diacritics insensitive, no stemming, no wildcards, no stop words),
   changed by the prolog's ft-option declarations in order, then by the
   FTPrimaryWithOptions around words, innermost first. */
static effective_options options_of(const sxml_xpath_lowerer *state, sxml_ast_node_index words) {
    effective_options effective = {.options = {.case_mode = SXML_FULL_TEXT_CASE_INSENSITIVE}};
    const sxml_ast_node_index root = state->ast->root;
    const sxml_ast_node_index prolog =
        node_of(state, root)->kind == SXML_AST_KIND_MODULE
            ? child_of(state, child_of(state, root, SXML_AST_FIELD_MODULE), SXML_AST_FIELD_PROLOG)
            : (sxml_ast_node_index){0};
    for (sxml_ast_node_index declaration = child_of(state, prolog, SXML_AST_FIELD_DECLARATION);
         prolog.value != 0 && declaration.value != 0;
         declaration = next_in(state->ast, declaration)) {
        if (node_of(state, declaration)->kind == SXML_AST_KIND_FT_OPTION_DECLARATION) {
            bool settled[SXML_FULL_TEXT_OPTION_GROUP_COUNT] = {false};
            take_options(state, (option_source){.owner = declaration, .settled = settled},
                         &effective);
        }
    }
    bool settled[SXML_FULL_TEXT_OPTION_GROUP_COUNT] = {false};
    for (sxml_ast_node_index ancestor = words;
         ancestor.value != 0 &&
         node_of(state, ancestor)->kind != SXML_AST_KIND_FT_CONTAINS_EXPRESSION;
         ancestor = node_of(state, ancestor)->parent) {
        if (node_of(state, ancestor)->kind == SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS) {
            take_options(state, (option_source){.owner = ancestor, .settled = settled},
                         &effective);
        }
    }
    return effective;
}

/* ------------------------------------------------- Stop words (3.4.7) */

/* Whether word is among words[0..word_count). */
static bool has_word(const sxml_byte_view *words, size_t word_count, sxml_byte_view word) {
    for (size_t word_index = 0; word_index < word_count; word_index += 1) {
        if (sxml_byte_view_equals(words[word_index], word)) {
            return true;
        }
    }
    return false;
}

/* Applies one FT_STOP_WORDS list to words: union adds its words, except
   removes them (left to right). */
typedef struct stop_word_list {
    sxml_byte_view *words;
    size_t word_count;
    size_t word_capacity;
} stop_word_list;

/* One FT_STOP_WORDS list and how it combines. */
typedef struct stop_word_change {
    sxml_ast_node_index list;
    bool is_except;
} stop_word_change;

static void apply_stop_words(const sxml_xpath_lowerer *state, stop_word_change change,
                             stop_word_list *words) {
    const bool is_except = change.is_except;
    for (sxml_ast_node_index word_node = child_of(state, change.list, SXML_AST_FIELD_WORD);
         word_node.value != 0; word_node = next_in(state->ast, word_node)) {
        const sxml_byte_view text =
            sxml_ast_get_text(state->ast, node_of(state, word_node)->payload.text);
        if (is_except) {
            for (size_t word_index = 0; word_index < words->word_count; word_index += 1) {
                if (sxml_byte_view_equals(words->words[word_index], text)) {
                    words->words[word_index] = words->words[words->word_count - 1];
                    words->word_count -= 1;
                    break;
                }
            }
        } else if (!has_word(words->words, words->word_count, text) &&
                   words->word_count < words->word_capacity) {
            words->words[words->word_count] = text;
            words->word_count += 1;
        }
    }
}

/* How many words an option's lists name at most. */
static size_t count_stop_words(const sxml_xpath_lowerer *state, sxml_ast_node_index option) {
    size_t word_count = 0;
    for (sxml_ast_node_index walked = option; walked.value != 0;) {
        const sxml_ast_node *current = node_of(state, walked);
        if (current->kind == SXML_AST_KIND_STRING_LITERAL) {
            word_count += 1;
        }
        if (current->first_child.value != 0) {
            walked = current->first_child;
            continue;
        }
        while (walked.value != option.value && node_of(state, walked)->next_sibling.value == 0) {
            walked = node_of(state, walked)->parent;
        }
        walked = walked.value == option.value ? (sxml_ast_node_index){0}
                                              : node_of(state, walked)->next_sibling;
    }
    return word_count;
}

/* Binds the stop words of option (an FT_STOP_WORD_OPTION) as an
   xs:string* sequence of constants in block. */
typedef struct stop_word_binding {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index option;
} stop_word_binding;

static sxml_status bind_stop_words(sxml_xpath_lowerer *state, stop_word_binding request,
                                   sxml_ir_atom *atom) {
    const size_t capacity = count_stop_words(state, request.option) + 1;
    stop_word_list words = {.word_capacity = capacity};
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        state,
        (sxml_xpath_lowering_scratch_request){.element_count = capacity,
                                              .element_size = sizeof(sxml_byte_view),
                                              .element_alignment = alignof(sxml_byte_view)},
        (void **)&words.words);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* "stop words default": sxml's default list is empty (3.4.7:
       implementation-defined). */
    apply_stop_words(state,
                     (stop_word_change){.list = child_of(state, request.option,
                                                         SXML_AST_FIELD_WORDS),
                                        .is_except = false},
                     &words);
    for (sxml_ast_node_index inclusion = child_of(state, request.option, SXML_AST_FIELD_INCLUSION);
         inclusion.value != 0; inclusion = next_in(state->ast, inclusion)) {
        apply_stop_words(
            state,
            (stop_word_change){.list = child_of(state, inclusion, SXML_AST_FIELD_WORDS),
                               .is_except = node_of(state, inclusion)->detail.operator==
                                            SXML_AST_OPERATOR_EXCEPT},
            &words);
    }
    sxml_ir_atom *constants = NULL;
    status = sxml_xpath_lowerer_allocate_scratch(
        state,
        (sxml_xpath_lowering_scratch_request){.element_count = capacity,
                                              .element_size = sizeof(sxml_ir_atom),
                                              .element_alignment = alignof(sxml_ir_atom)},
        (void **)&constants);
    for (size_t word_index = 0; status == SXML_STATUS_OK && word_index < words.word_count;
         word_index += 1) {
        status = sxml_xpath_lowerer_add_string_constant(state, words.words[word_index],
                                                        &constants[word_index]);
    }
    sxml_ir_atom_list list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            state->unit, (sxml_ir_atom_view){.atoms = constants, .atom_count = words.word_count},
            &list);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_new_expression(
            state,
            (expression_in_block){.block = request.block,
                                  .node = request.option,
                                  .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                                 .sequence = list}},
            atom);
    }
    sxml_xpath_lowerer_release_scratch(state, constants);
    sxml_xpath_lowerer_release_scratch(state, words.words);
    return status;
}

/* ------------------------------------------------------ The procedure */

/* The most operand expressions one node lowers: a words value and its
   range's two bounds. */
#define SXML_FULL_TEXT_MAXIMUM_PENDING 3U

/* The most bytes one instruction's text takes: an opcode, nine numbers
   of up to ten digits with their commas, and ";". */
#define SXML_FULL_TEXT_MAXIMUM_INSTRUCTION_BYTES 112U

/* The most program members one node adds: three operands and a stop
   word list. */
#define SXML_FULL_TEXT_MAXIMUM_NODE_MEMBERS 4U

/* What an operand is coerced to (4.3 step 1). */
typedef enum operand_type {
    SXML_FULL_TEXT_OPERAND_STRINGS,
    SXML_FULL_TEXT_OPERAND_INTEGER,
    SXML_FULL_TEXT_OPERAND_DOUBLE,
} operand_type;

typedef struct contains_locals {
    sxml_ir_atom search;
    sxml_ir_atom ignore;
    /* The program's nodes in post order (owned_memory[0]). */
    sxml_ast_node_index *nodes;
    size_t node_count;
    size_t node_position;
    /* The program's members (owned_memory[1]); members[0] is the text. */
    sxml_ir_atom *members;
    size_t member_count;
    /* The program text (owned_memory[2]). */
    uint8_t *text;
    size_t text_byte_count;
    /* The current node's operand expressions, lowered one per step. */
    sxml_ast_node_index pending[SXML_FULL_TEXT_MAXIMUM_PENDING];
    operand_type pending_types[SXML_FULL_TEXT_MAXIMUM_PENDING];
    sxml_ir_atom pending_atoms[SXML_FULL_TEXT_MAXIMUM_PENDING];
    size_t pending_count;
    size_t pending_next;
    bool is_planned;
} contains_locals;

enum {
    SXML_FULL_TEXT_STEP_SEARCH = 0,
    SXML_FULL_TEXT_STEP_PLAN = 1,
    SXML_FULL_TEXT_STEP_NODES = 2,
    SXML_FULL_TEXT_STEP_IGNORE = 3,
    SXML_FULL_TEXT_STEP_FINISH = 4,
};

/* Which children of a node have instructions, in program order. For an
   FT_SELECTION: its operand, then its FTOrder filters, then its other
   filters left to right (3.5: "FTOrder filters are applied first, and
   then the other positional filters are applied from left to right");
   for every other node, its program children in source order. */
typedef enum child_pass {
    SXML_FULL_TEXT_PASS_OPERAND,
    SXML_FULL_TEXT_PASS_ORDER,
    SXML_FULL_TEXT_PASS_OTHER_FILTERS,
} child_pass;

static child_pass pass_of(const sxml_xpath_lowerer *state, sxml_ast_node_index child) {
    const sxml_ast_node *node = node_of(state, child);
    if (node->field != SXML_AST_FIELD_FILTER) {
        return SXML_FULL_TEXT_PASS_OPERAND;
    }
    return node->kind == SXML_AST_KIND_FT_ORDER ? SXML_FULL_TEXT_PASS_ORDER
                                                : SXML_FULL_TEXT_PASS_OTHER_FILTERS;
}

/* The first program child of parent at or after from (0: the first
   child) in pass. */
typedef struct child_search {
    sxml_ast_node_index parent;
    sxml_ast_node_index from;
    child_pass pass;
} child_search;

static sxml_ast_node_index find_program_child(const sxml_xpath_lowerer *state,
                                              child_search search) {
    sxml_ast_node_index child =
        search.from.value != 0 ? search.from : node_of(state, search.parent)->first_child;
    const bool is_selection = node_of(state, search.parent)->kind == SXML_AST_KIND_FT_SELECTION;
    while (child.value != 0 &&
           (!is_program_kind(node_of(state, child)->kind) ||
            (is_selection && pass_of(state, child) != search.pass))) {
        child = node_of(state, child)->next_sibling;
    }
    return child;
}

/* The first child of node that has an instruction; 0 for none. */
static sxml_ast_node_index first_program_child(const sxml_xpath_lowerer *state,
                                               sxml_ast_node_index node) {
    for (uint32_t pass_number = SXML_FULL_TEXT_PASS_OPERAND;
         pass_number <= SXML_FULL_TEXT_PASS_OTHER_FILTERS; pass_number += 1) {
        const sxml_ast_node_index child = find_program_child(
            state, (child_search){.parent = node, .from = {0}, .pass = (child_pass)pass_number});
        if (child.value != 0) {
            return child;
        }
    }
    return (sxml_ast_node_index){0};
}

/* The program child after node among its parent's. */
static sxml_ast_node_index next_program_sibling(const sxml_xpath_lowerer *state,
                                                sxml_ast_node_index node) {
    const sxml_ast_node_index parent = node_of(state, node)->parent;
    const sxml_ast_node_index sibling = node_of(state, node)->next_sibling;
    if (node_of(state, parent)->kind != SXML_AST_KIND_FT_SELECTION) {
        return sibling.value == 0
                   ? sibling
                   : find_program_child(state, (child_search){.parent = parent,
                                                              .from = sibling,
                                                              .pass = SXML_FULL_TEXT_PASS_OPERAND});
    }
    for (uint32_t pass_number = pass_of(state, node);
         pass_number <= SXML_FULL_TEXT_PASS_OTHER_FILTERS; pass_number += 1) {
        const bool is_own_pass = pass_number == (uint32_t)pass_of(state, node);
        if (is_own_pass && sibling.value == 0) {
            continue;
        }
        const sxml_ast_node_index next = find_program_child(
            state, (child_search){.parent = parent,
                                  .from = is_own_pass ? sibling : (sxml_ast_node_index){0},
                                  .pass = (child_pass)pass_number});
        if (next.value != 0) {
            return next;
        }
    }
    return (sxml_ast_node_index){0};
}

static sxml_ast_node_index deepest_first(const sxml_xpath_lowerer *state,
                                         sxml_ast_node_index node) {
    sxml_ast_node_index current = node;
    for (sxml_ast_node_index child = first_program_child(state, current); child.value != 0;
         child = first_program_child(state, current)) {
        current = child;
    }
    return current;
}

/* Walks the selection rooted at root in post order, by the links (D71):
   counts its nodes, or writes them to nodes when it is not NULL. */
typedef struct post_order_walk {
    sxml_ast_node_index root;
    sxml_ast_node_index *nodes;
} post_order_walk;

static size_t walk_post_order(const sxml_xpath_lowerer *state, post_order_walk walk) {
    size_t node_count = 0;
    sxml_ast_node_index node = deepest_first(state, walk.root);
    while (node.value != 0) {
        if (walk.nodes != NULL) {
            walk.nodes[node_count] = node;
        }
        node_count += 1;
        if (node.value == walk.root.value) {
            break;
        }
        const sxml_ast_node_index sibling = next_program_sibling(state, node);
        node = sibling.value != 0 ? deepest_first(state, sibling) : node_of(state, node)->parent;
    }
    return node_count;
}

/* The names of node's extension options (3.4.8) or pragmas (3.6), which
   sxml recognizes none of, resolved for their errors (XPST0081 for an
   undeclared prefix). */
static sxml_status resolve_extension_names(sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    const sxml_ast_kind kind = node_of(state, node)->kind;
    if (kind != SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS &&
        kind != SXML_AST_KIND_FT_EXTENSION_SELECTION) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index child = node_of(state, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0; child = node_of(state, child)->next_sibling) {
        const sxml_ast_kind child_kind = node_of(state, child)->kind;
        if (child_kind != SXML_AST_KIND_FT_EXTENSION_OPTION && child_kind != SXML_AST_KIND_PRAGMA) {
            continue;
        }
        const sxml_ast_node_index name = child_of(state, child, SXML_AST_FIELD_NAME);
        if (node_of(state, name)->kind != SXML_AST_KIND_PREFIXED_NAME) {
            continue;
        }
        sxml_xpath_resolved_name resolved = {0};
        status = sxml_xpath_resolve_name(
            &(sxml_xpath_name_request){.ast = state->ast,
                                       .name_node = name,
                                       .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                       .static_context = state->static_context,
                                       .error_record = state->error_record},
            &resolved);
    }
    return status;
}

/* Allocates the node, member and text arrays and fills the nodes. */
static sxml_status plan_program(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    contains_locals *locals = frame->locals;
    const sxml_ast_node_index root = child_of(state, frame->node, SXML_AST_FIELD_SELECTION);
    locals->node_count = walk_post_order(state, (post_order_walk){.root = root, .nodes = NULL});
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 0,
            .scratch = {.element_count = locals->node_count,
                        .element_size = sizeof(sxml_ast_node_index),
                        .element_alignment = alignof(sxml_ast_node_index)}});
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 1,
                .scratch = {.element_count =
                                1 + locals->node_count * SXML_FULL_TEXT_MAXIMUM_NODE_MEMBERS,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 2,
                .scratch = {.element_count =
                                locals->node_count * SXML_FULL_TEXT_MAXIMUM_INSTRUCTION_BYTES,
                            .element_size = 1,
                            .element_alignment = 1}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->nodes = frame->owned_memory[0];
    locals->members = frame->owned_memory[1];
    locals->text = frame->owned_memory[2];
    locals->member_count = 1;
    walk_post_order(state, (post_order_walk){.root = root, .nodes = locals->nodes});
    for (size_t node_index = 0; status == SXML_STATUS_OK && node_index < locals->node_count;
         node_index += 1) {
        status = resolve_extension_names(state, locals->nodes[node_index]);
    }
    return status;
}

/* Adds an operand expression of the current node. */
static void add_pending(contains_locals *locals, sxml_ast_node_index node, operand_type type) {
    if (node.value == 0) {
        return;
    }
    locals->pending[locals->pending_count] = node;
    locals->pending_types[locals->pending_count] = type;
    locals->pending_count += 1;
}

/* The bounds of an FT_RANGE: "exactly" once, else minimum and maximum. */
static void add_range(const sxml_xpath_lowerer *state, contains_locals *locals,
                      sxml_ast_node_index range) {
    const sxml_ast_node_index exactly = child_of(state, range, SXML_AST_FIELD_VALUE);
    if (exactly.value != 0) {
        add_pending(locals, exactly, SXML_FULL_TEXT_OPERAND_INTEGER);
        return;
    }
    add_pending(locals, child_of(state, range, SXML_AST_FIELD_MINIMUM),
                SXML_FULL_TEXT_OPERAND_INTEGER);
    add_pending(locals, child_of(state, range, SXML_AST_FIELD_MAXIMUM),
                SXML_FULL_TEXT_OPERAND_INTEGER);
}

/* Lists the operand expressions of node (in the order the instructions
   name them). */
static void plan_node(const sxml_xpath_lowerer *state, contains_locals *locals,
                      sxml_ast_node_index node) {
    locals->pending_count = 0;
    locals->pending_next = 0;
    switch ((uint32_t)node_of(state, node)->kind) {
    case SXML_AST_KIND_FT_WORDS: {
        add_pending(locals, child_of(state, node, SXML_AST_FIELD_VALUE),
                    SXML_FULL_TEXT_OPERAND_STRINGS);
        const sxml_ast_node_index times = child_of(state, node, SXML_AST_FIELD_TIMES);
        if (times.value != 0) {
            add_range(state, locals, times);
        }
        break;
    }
    case SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS:
        add_pending(locals, child_of(state, node, SXML_AST_FIELD_WEIGHT),
                    SXML_FULL_TEXT_OPERAND_DOUBLE);
        break;
    case SXML_AST_KIND_FT_WINDOW:
        add_pending(locals, child_of(state, node, SXML_AST_FIELD_VALUE),
                    SXML_FULL_TEXT_OPERAND_INTEGER);
        break;
    case SXML_AST_KIND_FT_DISTANCE:
        add_range(state, locals, child_of(state, node, SXML_AST_FIELD_RANGE));
        break;
    default:
        break;
    }
}

/* Coerces a lowered operand (4.3 step 1: words as xs:string*, ranges
   and windows as xs:integer, weights as xs:double, each "as though it
   were an argument to a function") and adds it as the next member. */
typedef struct pending_operand {
    sxml_xpath_lowering_frame *frame;
    size_t pending_index;
} pending_operand;

static sxml_status add_member(sxml_xpath_lowerer *state, pending_operand operand,
                              uint32_t *member_number) {
    sxml_xpath_lowering_frame *frame = operand.frame;
    const size_t pending_index = operand.pending_index;
    contains_locals *locals = frame->locals;
    const operand_type type = locals->pending_types[pending_index];
    const sxml_sequence_type sequence_type = {
        .item_type = sxml_builtin_type_identifier(type == SXML_FULL_TEXT_OPERAND_STRINGS
                                                      ? SXML_TYPE_XS_STRING
                                                  : type == SXML_FULL_TEXT_OPERAND_INTEGER
                                                      ? SXML_TYPE_XS_INTEGER
                                                      : SXML_TYPE_XS_DOUBLE),
        .occurrence = type == SXML_FULL_TEXT_OPERAND_STRINGS ? SXML_OCCURRENCE_ZERO_OR_MORE
                                                             : SXML_OCCURRENCE_EXACTLY_ONE};
    const sxml_status status = sxml_xpath_lowerer_coerce_atom(
        state,
        (sxml_xpath_lowering_coercion_request){.block = frame->block,
                                               .node = locals->pending[pending_index],
                                               .value = locals->pending_atoms[pending_index],
                                               .sequence_type = sequence_type,
                                               .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
        &locals->members[locals->member_count]);
    if (status == SXML_STATUS_OK) {
        locals->member_count += 1;
        *member_number = (uint32_t)locals->member_count;
    }
    return status;
}

/* Appends an instruction: opcode and its arguments. */
typedef struct instruction_text {
    uint8_t opcode;
    const uint32_t *arguments;
    size_t argument_count;
} instruction_text;

static void append_instruction(contains_locals *locals, instruction_text instruction) {
    locals->text[locals->text_byte_count] = instruction.opcode;
    locals->text_byte_count += 1;
    for (size_t argument = 0; argument < instruction.argument_count; argument += 1) {
        if (argument != 0) {
            locals->text[locals->text_byte_count] = ',';
            locals->text_byte_count += 1;
        }
        uint8_t digits[10];
        size_t digit_count = 0;
        uint32_t value = instruction.arguments[argument];
        do {
            digits[digit_count] = (uint8_t)('0' + value % 10U);
            digit_count += 1;
            value /= 10U;
        } while (value != 0);
        while (digit_count > 0) {
            digit_count -= 1;
            locals->text[locals->text_byte_count] = digits[digit_count];
            locals->text_byte_count += 1;
        }
    }
    locals->text[locals->text_byte_count] = ';';
    locals->text_byte_count += 1;
}

static uint32_t unit_of(sxml_ast_keyword keyword) {
    return keyword == SXML_AST_KEYWORD_SENTENCES  ? (uint32_t)SXML_FULL_TEXT_UNIT_SENTENCES
           : keyword == SXML_AST_KEYWORD_PARAGRAPHS ? (uint32_t)SXML_FULL_TEXT_UNIT_PARAGRAPHS
                                                    : (uint32_t)SXML_FULL_TEXT_UNIT_WORDS;
}

static uint32_t any_all_of(const sxml_ast_node *words) {
    const bool is_separate = (words->flags & SXML_AST_FLAG_SEPARATE_WORDS) != 0;
    switch ((uint32_t)words->detail.keyword) {
    case SXML_AST_KEYWORD_ALL:
        return is_separate ? (uint32_t)SXML_FULL_TEXT_ANY_ALL_ALL_WORDS
                           : (uint32_t)SXML_FULL_TEXT_ANY_ALL_ALL;
    case SXML_AST_KEYWORD_PHRASE:
        return (uint32_t)SXML_FULL_TEXT_ANY_ALL_PHRASE;
    default:
        return is_separate ? (uint32_t)SXML_FULL_TEXT_ANY_ALL_ANY_WORD
                           : (uint32_t)SXML_FULL_TEXT_ANY_ALL_ANY;
    }
}

/* The range bounds of a node's instruction: (minimum, maximum) member
   numbers from the operands at members[first..]; "exactly" names one
   member twice. */
typedef struct range_members {
    uint32_t minimum;
    uint32_t maximum;
} range_members;

static range_members range_members_of(const sxml_xpath_lowerer *state, sxml_ast_node_index range,
                                      const uint32_t *numbers) {
    if (child_of(state, range, SXML_AST_FIELD_VALUE).value != 0) {
        return (range_members){.minimum = numbers[0], .maximum = numbers[0]};
    }
    const bool has_minimum = child_of(state, range, SXML_AST_FIELD_MINIMUM).value != 0;
    const bool has_maximum = child_of(state, range, SXML_AST_FIELD_MAXIMUM).value != 0;
    return (range_members){.minimum = has_minimum ? numbers[0] : 0,
                           .maximum = has_maximum ? numbers[has_minimum ? 1 : 0] : 0};
}

/* The instructions of words: W, then T for "occurs ... times". */
static sxml_status emit_words(sxml_xpath_lowerer *state, sxml_xpath_lowering_frame *frame,
                              const uint32_t *numbers) {
    contains_locals *locals = frame->locals;
    const sxml_ast_node_index node = locals->nodes[locals->node_position];
    const effective_options effective = options_of(state, node);
    uint32_t stop_words = 0;
    if (effective.stop_words.value != 0) {
        const sxml_status status = bind_stop_words(
            state, (stop_word_binding){.block = frame->block, .option = effective.stop_words},
            &locals->members[locals->member_count]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->member_count += 1;
        stop_words = (uint32_t)locals->member_count;
    }
    const sxml_full_text_match_options *options = &effective.options;
    const uint32_t arguments[SXML_FULL_TEXT_MAXIMUM_ARGUMENT_COUNT] = {
        numbers[0],
        any_all_of(node_of(state, node)),
        (uint32_t)options->case_mode,
        options->is_diacritics_sensitive ? 1U : 0U,
        options->uses_stemming ? 1U : 0U,
        options->uses_wildcards ? 1U : 0U,
        0,
        stop_words,
        0};
    append_instruction(locals, (instruction_text){.opcode = SXML_FULL_TEXT_OPCODE_WORDS,
                                                  .arguments = arguments,
                                                  .argument_count =
                                                      SXML_FULL_TEXT_MAXIMUM_ARGUMENT_COUNT});
    const sxml_ast_node_index times = child_of(state, node, SXML_AST_FIELD_TIMES);
    if (times.value != 0) {
        const range_members range = range_members_of(state, times, numbers + 1);
        const uint32_t bounds[2] = {range.minimum, range.maximum};
        append_instruction(locals, (instruction_text){.opcode = SXML_FULL_TEXT_OPCODE_TIMES,
                                                      .arguments = bounds,
                                                      .argument_count = 2});
    }
    return SXML_STATUS_OK;
}

/* The instruction of the current node, its operands lowered and added
   as members whose numbers are in numbers. */
static sxml_status emit_node(sxml_xpath_lowerer *state, sxml_xpath_lowering_frame *frame,
                             const uint32_t *numbers) {
    contains_locals *locals = frame->locals;
    const sxml_ast_node_index node = locals->nodes[locals->node_position];
    const sxml_ast_node *ast_node = node_of(state, node);
    uint32_t arguments[3] = {0, 0, 0};
    instruction_text instruction = {.opcode = 0, .arguments = arguments, .argument_count = 0};
    switch ((uint32_t)ast_node->kind) {
    case SXML_AST_KIND_FT_WORDS:
        return emit_words(state, frame, numbers);
    case SXML_AST_KIND_FT_OR:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_OR;
        break;
    case SXML_AST_KIND_FT_AND:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_AND;
        break;
    case SXML_AST_KIND_FT_MILD_NOT:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_MILD_NOT;
        break;
    case SXML_AST_KIND_FT_UNARY_NOT:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_NOT;
        break;
    case SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS:
        if (locals->pending_count == 0) {
            return SXML_STATUS_OK;
        }
        instruction.opcode = SXML_FULL_TEXT_OPCODE_WEIGHT;
        arguments[0] = numbers[0];
        instruction.argument_count = 1;
        break;
    case SXML_AST_KIND_FT_ORDER:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_ORDER;
        break;
    case SXML_AST_KIND_FT_WINDOW:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_WINDOW;
        arguments[0] = numbers[0];
        arguments[1] = unit_of(ast_node->detail.keyword);
        instruction.argument_count = 2;
        break;
    case SXML_AST_KIND_FT_DISTANCE: {
        const range_members range =
            range_members_of(state, child_of(state, node, SXML_AST_FIELD_RANGE), numbers);
        instruction.opcode = SXML_FULL_TEXT_OPCODE_DISTANCE;
        arguments[0] = range.minimum;
        arguments[1] = range.maximum;
        arguments[2] = unit_of(ast_node->detail.keyword);
        instruction.argument_count = 3;
        break;
    }
    case SXML_AST_KIND_FT_SCOPE:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_SCOPE;
        arguments[0] = (ast_node->flags & SXML_AST_FLAG_DIFFERENT) != 0 ? 1U : 0U;
        arguments[1] = ast_node->detail.keyword == SXML_AST_KEYWORD_PARAGRAPH ? 1U : 0U;
        instruction.argument_count = 2;
        break;
    case SXML_AST_KIND_FT_CONTENT:
        instruction.opcode = SXML_FULL_TEXT_OPCODE_CONTENT;
        arguments[0] = ast_node->detail.keyword == SXML_AST_KEYWORD_START
                           ? (uint32_t)SXML_FULL_TEXT_CONTENT_AT_START
                       : ast_node->detail.keyword == SXML_AST_KEYWORD_END
                           ? (uint32_t)SXML_FULL_TEXT_CONTENT_AT_END
                           : (uint32_t)SXML_FULL_TEXT_CONTENT_ENTIRE;
        instruction.argument_count = 1;
        break;
    default:
        /* FT_SELECTION and FT_EXTENSION_SELECTION: their children's
           instructions are theirs. */
        return SXML_STATUS_OK;
    }
    append_instruction(locals, instruction);
    return SXML_STATUS_OK;
}

/* One step over the program's nodes: the next operand to lower, or the
   current node's instruction and the next node. */
static sxml_status continue_nodes(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    contains_locals *locals = frame->locals;
    while (locals->node_position < locals->node_count) {
        if (!locals->is_planned) {
            plan_node(state, locals, locals->nodes[locals->node_position]);
            locals->is_planned = true;
        }
        if (locals->pending_next < locals->pending_count) {
            const size_t pending_index = locals->pending_next;
            locals->pending_next += 1;
            return sxml_xpath_lowering_call_atom(
                machine,
                (sxml_xpath_lowering_atom_call){.node = locals->pending[pending_index],
                                                .block = frame->block,
                                                .atom = &locals->pending_atoms[pending_index]});
        }
        uint32_t numbers[SXML_FULL_TEXT_MAXIMUM_PENDING] = {0, 0, 0};
        sxml_status status = SXML_STATUS_OK;
        for (size_t pending_index = 0;
             status == SXML_STATUS_OK && pending_index < locals->pending_count;
             pending_index += 1) {
            status = add_member(
                state, (pending_operand){.frame = frame, .pending_index = pending_index},
                &numbers[pending_index]);
        }
        if (status == SXML_STATUS_OK) {
            status = emit_node(state, frame, numbers);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->node_position += 1;
        locals->is_planned = false;
    }
    frame->step = SXML_FULL_TEXT_STEP_IGNORE;
    return SXML_STATUS_OK;
}

/* The program array, then contains(search, program, ignore). */
static sxml_status finish_contains(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    contains_locals *locals = frame->locals;
    sxml_status status = sxml_xpath_lowerer_add_string_constant(
        state, (sxml_byte_view){.bytes = locals->text, .byte_count = locals->text_byte_count},
        &locals->members[0]);
    sxml_ir_atom_list members = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            state->unit,
            (sxml_ir_atom_view){.atoms = locals->members, .atom_count = locals->member_count},
            &members);
    }
    sxml_ir_atom program = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_new_expression(
            state,
            (expression_in_block){
                .block = frame->block,
                .node = frame->node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                               .array_constructor = {.array_form = SXML_IR_ARRAY_FORM_SQUARE,
                                                     .members = members}}},
            &program);
    }
    if (status == SXML_STATUS_OK &&
        child_of(state, frame->node, SXML_AST_FIELD_IGNORE).value == 0) {
        status = bind_new_expression(
            state,
            (expression_in_block){.block = frame->block,
                                  .node = frame->node,
                                  .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                                 .sequence = {0}}},
            &locals->ignore);
    }
    sxml_ir_expression expression;
    if (status == SXML_STATUS_OK) {
        const sxml_ir_atom arguments[3] = {locals->search, program, locals->ignore};
        status = make_native_call(state,
                                  (native_call){.node = frame->node,
                                                .native_function = &sxml_full_text_contains_function,
                                                .arguments = {.atoms = arguments, .atom_count = 3}},
                                  &expression);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){.node = frame->node, .expression = expression},
            &result);
    }
    return status == SXML_STATUS_OK ? sxml_xpath_lowering_finish_frame(machine, frame, result)
                                    : status;
}

_Static_assert(sizeof(contains_locals) <= SXML_FULL_TEXT_LOWERING_LOCALS_SIZE,
               "the core's contains procedure reserves the locals");

sxml_status sxml_full_text_lower_step(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    contains_locals *locals = frame->locals;
    switch (frame->step) {
    case SXML_FULL_TEXT_STEP_SEARCH:
        frame->step = SXML_FULL_TEXT_STEP_PLAN;
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = child_of(state, frame->node, SXML_AST_FIELD_VALUE),
                         .block = frame->block,
                         .atom = &locals->search});
    case SXML_FULL_TEXT_STEP_PLAN: {
        const sxml_status status = plan_program(machine, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->step = SXML_FULL_TEXT_STEP_NODES;
        return continue_nodes(machine, frame);
    }
    case SXML_FULL_TEXT_STEP_NODES:
        return continue_nodes(machine, frame);
    case SXML_FULL_TEXT_STEP_IGNORE: {
        const sxml_ast_node_index ignore = child_of(state, frame->node, SXML_AST_FIELD_IGNORE);
        frame->step = SXML_FULL_TEXT_STEP_FINISH;
        if (ignore.value != 0) {
            return sxml_xpath_lowering_call_atom(
                machine, (sxml_xpath_lowering_atom_call){
                             .node = ignore, .block = frame->block, .atom = &locals->ignore});
        }
        return finish_contains(machine, frame);
    }
    default:
        return finish_contains(machine, frame);
    }
}
