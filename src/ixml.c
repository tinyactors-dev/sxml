/*
 * Invisible XML grammars and the XML of a parse (src/ixml_internal.h;
 * Invisible XML Specification 1.0 with the errata of the editorial draft
 * of 2026-02-03; packet 7.18, D215).
 *
 * - The grammar builder reads the XML form of a grammar ("IXML in XML")
 *   into description nodes, from tree builder events or from the
 *   constant table of the grammar of grammars, and compiles them: each
 *   rule becomes a nonterminal; groups, options and repetitions become
 *   hidden nonterminals the compiler makes ("Hints for Implementers",
 *   left-recursive so that Earley's algorithm reads them in linear time);
 *   a quoted string becomes one terminal per character.
 * - Construction sends a parse tree to a tree builder ("Constructing
 *   XML"): marks, aliases, exposed attributes and insertions, and the
 *   errors D01 to D07.
 *
 * No recursion (D71): the compiler takes pending nonterminals from a
 * list, and every walk over description nodes or the parse tree follows
 * parent, child and sibling links.
 *
 * Golden file: src/html.c (an input mapped to tree builder events, and
 * an optional feature's definition).
 */
#include "ixml_internal.h"

#include "function_library_internal.h"
#include "memory_internal.h"
#include "optional_feature_internal.h"
#include "regex_internal.h"

#include <stdlib.h>
#include <string.h>

/* The ixml namespace (the Specification, "Prolog"). */
#define SXML_IXML_NAMESPACE_URI "http://invisiblexml.org/NS"

/* --------------------------------------------------------- Grammars */

void sxml_ixml_grammar_finalize(sxml_ixml_grammar *grammar) {
    sxml_xml_buffer_release(&grammar->text);
    sxml_xml_array_release(&grammar->nonterminals);
    sxml_xml_array_release(&grammar->productions);
    sxml_xml_array_release(&grammar->symbols);
    sxml_xml_array_release(&grammar->terminals);
    sxml_xml_array_release(&grammar->ranges);
    sxml_ixml_parse_tables_release(&grammar->tables);
    grammar->has_version_mismatch = false;
}

sxml_byte_view sxml_ixml_grammar_text(const sxml_ixml_grammar *grammar,
                                      sxml_ixml_text_range range) {
    if (range.byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = grammar->text.bytes + range.byte_offset,
                            .byte_count = range.byte_count};
}

bool sxml_ixml_terminal_matches(sxml_ixml_terminal_match match, uint32_t codepoint) {
    const sxml_ixml_terminal *terminal =
        sxml_xml_array_at(&match.grammar->terminals, match.terminal);
    bool is_member = false;
    for (uint32_t range_index = 0; !is_member && range_index < terminal->range_count;
         range_index += 1) {
        const sxml_ixml_codepoint_range *range =
            sxml_xml_array_at(&match.grammar->ranges, terminal->first_range + range_index);
        is_member = codepoint >= range->first_codepoint && codepoint <= range->last_codepoint;
    }
    if (!is_member && terminal->category_mask != 0) {
        is_member = (terminal->category_mask &
                     (1U << (uint32_t)sxml_regex_get_general_category(codepoint))) != 0;
    }
    return is_member != terminal->is_exclusion;
}

/* -------------------------------------------------- Grammar builder */

/* An element of the XML form as read. */
typedef struct description_node {
    uint8_t element_kind; /* sxml_ixml_element_kind */
    uint32_t parent;
    uint32_t first_child;
    uint32_t last_child;
    uint32_t next_sibling;
    /* Bit k: attribute kind k is present. */
    uint32_t attribute_mask;
    sxml_ixml_text_range attributes[SXML_IXML_ATTRIBUTE_KIND_COUNT];
} description_node;

static description_node *description_at(const sxml_ixml_grammar_builder *builder,
                                        uint32_t node_index) {
    return sxml_xml_array_at(&builder->nodes, node_index);
}

void sxml_ixml_grammar_builder_initialize(
    sxml_ixml_grammar_builder *builder,
    const sxml_ixml_grammar_builder_configuration *configuration) {
    *builder = (sxml_ixml_grammar_builder){
        .allocator = configuration->allocator,
        .name_pool = configuration->name_pool,
        .text = sxml_xml_buffer_empty(configuration->allocator),
        .nodes = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = configuration->allocator,
            .element_byte_count = sizeof(description_node)}),
        .open_node = SXML_IXML_NONE,
        .ignored_depth = 0,
        .error = SXML_IXML_GRAMMAR_ERROR_NONE};
}

void sxml_ixml_grammar_builder_finalize(sxml_ixml_grammar_builder *builder) {
    sxml_xml_buffer_release(&builder->text);
    sxml_xml_array_release(&builder->nodes);
}

static sxml_status read_start(sxml_ixml_grammar_builder *builder,
                              sxml_ixml_element_kind element_kind) {
    if (builder->ignored_depth > 0) {
        builder->ignored_depth += 1;
        return SXML_STATUS_OK;
    }
    if (element_kind == SXML_IXML_ELEMENT_KIND_COMMENT ||
        element_kind == SXML_IXML_ELEMENT_KIND_UNKNOWN) {
        if (element_kind == SXML_IXML_ELEMENT_KIND_UNKNOWN) {
            builder->error = SXML_IXML_GRAMMAR_ERROR_FORM;
        }
        builder->ignored_depth = 1;
        return SXML_STATUS_OK;
    }
    if (builder->nodes.element_count >= SXML_IXML_NONE - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (builder->open_node == SXML_IXML_NONE && builder->nodes.element_count > 0) {
        /* A second top-level element. */
        builder->error = SXML_IXML_GRAMMAR_ERROR_FORM;
    }
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&builder->nodes, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t node_index = (uint32_t)(builder->nodes.element_count - 1);
    description_node *node = element;
    *node = (description_node){.element_kind = (uint8_t)element_kind,
                               .parent = builder->open_node,
                               .first_child = SXML_IXML_NONE,
                               .last_child = SXML_IXML_NONE,
                               .next_sibling = SXML_IXML_NONE,
                               .attribute_mask = 0};
    if (builder->open_node != SXML_IXML_NONE) {
        description_node *parent = description_at(builder, builder->open_node);
        if (parent->last_child == SXML_IXML_NONE) {
            parent->first_child = node_index;
        } else {
            description_at(builder, parent->last_child)->next_sibling = node_index;
        }
        parent->last_child = node_index;
    }
    builder->open_node = node_index;
    return SXML_STATUS_OK;
}

/* An attribute of the open element. */
typedef struct read_attribute_request {
    sxml_ixml_attribute_kind attribute_kind;
    sxml_byte_view value;
} read_attribute_request;

static sxml_status read_attribute(sxml_ixml_grammar_builder *builder,
                                  const read_attribute_request *request) {
    if (builder->ignored_depth > 0 || request->attribute_kind == SXML_IXML_ATTRIBUTE_KIND_NONE) {
        return SXML_STATUS_OK;
    }
    if (builder->open_node == SXML_IXML_NONE) {
        builder->error = SXML_IXML_GRAMMAR_ERROR_FORM;
        return SXML_STATUS_OK;
    }
    if (builder->text.byte_count > UINT32_MAX - request->value.byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const uint32_t byte_offset = (uint32_t)builder->text.byte_count;
    const sxml_status status = sxml_xml_buffer_append(&builder->text, request->value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    description_node *node = description_at(builder, builder->open_node);
    node->attribute_mask |= 1U << (uint32_t)request->attribute_kind;
    node->attributes[request->attribute_kind] = (sxml_ixml_text_range){
        .byte_offset = byte_offset, .byte_count = (uint32_t)request->value.byte_count};
    return SXML_STATUS_OK;
}

static void read_end(sxml_ixml_grammar_builder *builder) {
    if (builder->ignored_depth > 0) {
        builder->ignored_depth -= 1;
        return;
    }
    if (builder->open_node != SXML_IXML_NONE) {
        builder->open_node = description_at(builder, builder->open_node)->parent;
    }
}

sxml_status sxml_ixml_grammar_builder_read_entries(sxml_ixml_grammar_builder *builder,
                                                   sxml_ixml_grammar_entry_view entries) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entries.entry_count;
         entry_index += 1) {
        const sxml_ixml_grammar_entry *entry = &entries.entries[entry_index];
        switch ((sxml_ixml_entry_kind)entry->entry_kind) {
        case SXML_IXML_ENTRY_KIND_START:
            status = read_start(builder, (sxml_ixml_element_kind)entry->element_kind);
            break;
        case SXML_IXML_ENTRY_KIND_ATTRIBUTE:
            status = read_attribute(
                builder,
                &(read_attribute_request){
                    .attribute_kind = (sxml_ixml_attribute_kind)entry->attribute_kind,
                    .value = entry->attribute_value});
            break;
        case SXML_IXML_ENTRY_KIND_END:
            read_end(builder);
            break;
        }
    }
    return status;
}

/* The names of the XML form, by kind. */
static const sxml_byte_view element_names[SXML_IXML_ELEMENT_KIND_COUNT] = {
    [SXML_IXML_ELEMENT_KIND_UNKNOWN] = {.bytes = NULL, .byte_count = 0},
    [SXML_IXML_ELEMENT_KIND_IXML] = SXML_BYTE_VIEW_INITIALIZER("ixml"),
    [SXML_IXML_ELEMENT_KIND_PROLOG] = SXML_BYTE_VIEW_INITIALIZER("prolog"),
    [SXML_IXML_ELEMENT_KIND_VERSION] = SXML_BYTE_VIEW_INITIALIZER("version"),
    [SXML_IXML_ELEMENT_KIND_RULE] = SXML_BYTE_VIEW_INITIALIZER("rule"),
    [SXML_IXML_ELEMENT_KIND_ALT] = SXML_BYTE_VIEW_INITIALIZER("alt"),
    [SXML_IXML_ELEMENT_KIND_ALTS] = SXML_BYTE_VIEW_INITIALIZER("alts"),
    [SXML_IXML_ELEMENT_KIND_NONTERMINAL] = SXML_BYTE_VIEW_INITIALIZER("nonterminal"),
    [SXML_IXML_ELEMENT_KIND_LITERAL] = SXML_BYTE_VIEW_INITIALIZER("literal"),
    [SXML_IXML_ELEMENT_KIND_INCLUSION] = SXML_BYTE_VIEW_INITIALIZER("inclusion"),
    [SXML_IXML_ELEMENT_KIND_EXCLUSION] = SXML_BYTE_VIEW_INITIALIZER("exclusion"),
    [SXML_IXML_ELEMENT_KIND_MEMBER] = SXML_BYTE_VIEW_INITIALIZER("member"),
    [SXML_IXML_ELEMENT_KIND_INSERTION] = SXML_BYTE_VIEW_INITIALIZER("insertion"),
    [SXML_IXML_ELEMENT_KIND_OPTION] = SXML_BYTE_VIEW_INITIALIZER("option"),
    [SXML_IXML_ELEMENT_KIND_REPEAT0] = SXML_BYTE_VIEW_INITIALIZER("repeat0"),
    [SXML_IXML_ELEMENT_KIND_REPEAT1] = SXML_BYTE_VIEW_INITIALIZER("repeat1"),
    [SXML_IXML_ELEMENT_KIND_SEP] = SXML_BYTE_VIEW_INITIALIZER("sep"),
    [SXML_IXML_ELEMENT_KIND_COMMENT] = SXML_BYTE_VIEW_INITIALIZER("comment"),
};

static const sxml_byte_view attribute_names[SXML_IXML_ATTRIBUTE_KIND_COUNT] = {
    [SXML_IXML_ATTRIBUTE_KIND_NONE] = {.bytes = NULL, .byte_count = 0},
    [SXML_IXML_ATTRIBUTE_KIND_NAME] = SXML_BYTE_VIEW_INITIALIZER("name"),
    [SXML_IXML_ATTRIBUTE_KIND_MARK] = SXML_BYTE_VIEW_INITIALIZER("mark"),
    [SXML_IXML_ATTRIBUTE_KIND_ALIAS] = SXML_BYTE_VIEW_INITIALIZER("alias"),
    [SXML_IXML_ATTRIBUTE_KIND_TMARK] = SXML_BYTE_VIEW_INITIALIZER("tmark"),
    [SXML_IXML_ATTRIBUTE_KIND_STRING] = SXML_BYTE_VIEW_INITIALIZER("string"),
    [SXML_IXML_ATTRIBUTE_KIND_HEX] = SXML_BYTE_VIEW_INITIALIZER("hex"),
    [SXML_IXML_ATTRIBUTE_KIND_FROM] = SXML_BYTE_VIEW_INITIALIZER("from"),
    [SXML_IXML_ATTRIBUTE_KIND_TO] = SXML_BYTE_VIEW_INITIALIZER("to"),
    [SXML_IXML_ATTRIBUTE_KIND_CODE] = SXML_BYTE_VIEW_INITIALIZER("code"),
};

/* The local name of a name in no namespace, or an empty view for a name
   in a namespace (not part of the grammar). */
static sxml_byte_view local_name_in_no_namespace(const sxml_ixml_grammar_builder *builder,
                                                 sxml_name_identifier name) {
    sxml_name_parts parts;
    if (builder->name_pool == NULL ||
        sxml_name_pool_get_name_parts(builder->name_pool, name, &parts) != SXML_STATUS_OK ||
        parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return parts.local_name;
}

static sxml_status builder_start_document(void *state, const sxml_document_start_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status builder_end_document(void *state) {
    (void)state;
    return SXML_STATUS_OK;
}

static sxml_status builder_start_element(void *state, const sxml_element_start_event *event) {
    sxml_ixml_grammar_builder *builder = state;
    const sxml_byte_view local_name = local_name_in_no_namespace(builder, event->element_name);
    if (local_name.byte_count == 0) {
        /* An element in a namespace is left out with its content. */
        if (builder->ignored_depth == 0) {
            builder->ignored_depth = 1;
        } else {
            builder->ignored_depth += 1;
        }
        return SXML_STATUS_OK;
    }
    sxml_ixml_element_kind element_kind = SXML_IXML_ELEMENT_KIND_UNKNOWN;
    for (size_t kind_index = 1; kind_index < SXML_IXML_ELEMENT_KIND_COUNT; kind_index += 1) {
        if (sxml_byte_view_equals(element_names[kind_index], local_name)) {
            element_kind = (sxml_ixml_element_kind)kind_index;
        }
    }
    return read_start(builder, element_kind);
}

static sxml_status builder_add_namespace_binding(void *state,
                                                 const sxml_namespace_binding *namespace_binding) {
    (void)state;
    (void)namespace_binding;
    return SXML_STATUS_OK;
}

static sxml_status builder_add_attribute(void *state, const sxml_attribute_event *event) {
    sxml_ixml_grammar_builder *builder = state;
    const sxml_byte_view local_name = local_name_in_no_namespace(builder, event->attribute_name);
    sxml_ixml_attribute_kind attribute_kind = SXML_IXML_ATTRIBUTE_KIND_NONE;
    for (size_t kind_index = 1; local_name.byte_count > 0 &&
                                kind_index < SXML_IXML_ATTRIBUTE_KIND_COUNT;
         kind_index += 1) {
        if (sxml_byte_view_equals(attribute_names[kind_index], local_name)) {
            attribute_kind = (sxml_ixml_attribute_kind)kind_index;
        }
    }
    return read_attribute(builder,
                          &(read_attribute_request){.attribute_kind = attribute_kind,
                                                    .value = event->attribute_value});
}

static sxml_status builder_end_element(void *state) {
    read_end(state);
    return SXML_STATUS_OK;
}

static sxml_status builder_add_text(void *state, const sxml_text_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status builder_add_comment(void *state, const sxml_comment_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status builder_add_processing_instruction(
    void *state, const sxml_processing_instruction_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status builder_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    (void)state;
    (void)entity;
    return SXML_STATUS_OK;
}

static const sxml_tree_builder_operations grammar_builder_operations = {
    .start_document = builder_start_document,
    .end_document = builder_end_document,
    .start_element = builder_start_element,
    .add_namespace_binding = builder_add_namespace_binding,
    .add_attribute = builder_add_attribute,
    .end_element = builder_end_element,
    .add_text = builder_add_text,
    .add_comment = builder_add_comment,
    .add_processing_instruction = builder_add_processing_instruction,
    .add_unparsed_entity = builder_add_unparsed_entity,
};

sxml_tree_builder sxml_ixml_grammar_builder_as_tree_builder(sxml_ixml_grammar_builder *builder) {
    return (sxml_tree_builder){.state = builder, .operations = &grammar_builder_operations};
}

/* --------------------------------------------------------- Compiling */

/* What a pending nonterminal is made of. */
typedef enum sxml_ixml_construct_kind {
    /* A rule or a group: one production per alt child. */
    SXML_IXML_CONSTRUCT_KIND_ALTERNATIVES,
    /* f? : () | f. */
    SXML_IXML_CONSTRUCT_KIND_OPTION,
    /* f* : () | G, f; f**sep : () | f++sep. */
    SXML_IXML_CONSTRUCT_KIND_REPEAT0,
    /* f+ : f | G, f; f++sep : f | G, sep, f. */
    SXML_IXML_CONSTRUCT_KIND_REPEAT1,
} sxml_ixml_construct_kind;

/* A nonterminal whose productions are still to compile. */
typedef struct pending_nonterminal {
    uint32_t nonterminal;
    uint32_t node;
    sxml_ixml_construct_kind kind;
} pending_nonterminal;

/* A rule's name and nonterminal, for lookups by name. */
typedef struct rule_name {
    sxml_byte_view name;
    uint32_t nonterminal;
} rule_name;

typedef struct grammar_compiler {
    sxml_ixml_grammar_builder *builder;
    sxml_ixml_grammar *grammar;
    sxml_xml_array pending;     /* pending_nonterminal */
    sxml_xml_array productions; /* sxml_ixml_production, unsorted */
    rule_name *rule_names;      /* sorted by name */
    size_t rule_count;
    sxml_ixml_grammar_error error;
    /* The name the first S02 or S03 is about, in the builder's text. */
    sxml_byte_view error_name;
} grammar_compiler;

static bool has_attribute(const description_node *node, sxml_ixml_attribute_kind attribute_kind) {
    return (node->attribute_mask & (1U << (uint32_t)attribute_kind)) != 0;
}

static sxml_byte_view attribute_value(const grammar_compiler *compiler,
                                      const description_node *node,
                                      sxml_ixml_attribute_kind attribute_kind) {
    if (!has_attribute(node, attribute_kind)) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    const sxml_ixml_text_range range = node->attributes[attribute_kind];
    return (sxml_byte_view){.bytes = compiler->builder->text.bytes + range.byte_offset,
                            .byte_count = range.byte_count};
}

static void fail_compiling(grammar_compiler *compiler, sxml_ixml_grammar_error error) {
    if (compiler->error == SXML_IXML_GRAMMAR_ERROR_NONE) {
        compiler->error = error;
    }
}

/* Copies text into the grammar's text. */
static sxml_status store_text(grammar_compiler *compiler, sxml_byte_view text,
                              sxml_ixml_text_range *range) {
    if (compiler->grammar->text.byte_count > UINT32_MAX - text.byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    *range = (sxml_ixml_text_range){.byte_offset = (uint32_t)compiler->grammar->text.byte_count,
                                    .byte_count = (uint32_t)text.byte_count};
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_xml_buffer_append(&compiler->grammar->text, text);
}

static sxml_status push_element(sxml_xml_array *array, const void *value, uint32_t *index) {
    if (array->element_count >= SXML_IXML_NONE - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(array, &element);
    if (status == SXML_STATUS_OK) {
        memcpy(element, value, array->element_byte_count);
        if (index != NULL) {
            *index = (uint32_t)(array->element_count - 1);
        }
    }
    return status;
}

/* A mark attribute's value; FORM for any other text. */
static sxml_ixml_mark read_mark(grammar_compiler *compiler, sxml_byte_view text,
                                bool allows_attribute) {
    if (text.byte_count == 1) {
        switch (text.bytes[0]) {
        case '^':
            return SXML_IXML_MARK_ELEMENT;
        case '-':
            return SXML_IXML_MARK_HIDDEN;
        case '@':
            if (allows_attribute) {
                return SXML_IXML_MARK_ATTRIBUTE;
            }
            break;
        default:
            break;
        }
    } else if (text.byte_count == 0) {
        return SXML_IXML_MARK_NONE;
    }
    fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
    return SXML_IXML_MARK_NONE;
}

/* A nonterminal's mark: "@", "^" or "-". */
typedef struct mark_request {
    sxml_byte_view text;
} mark_request;

static sxml_ixml_mark read_nonterminal_mark(grammar_compiler *compiler, mark_request request) {
    return read_mark(compiler, request.text, true);
}

/* A terminal's mark: "^" or "-". */
static sxml_ixml_mark read_terminal_mark(grammar_compiler *compiler, mark_request request) {
    return read_mark(compiler, request.text, false);
}

static bool is_hex_digit(uint8_t byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
           (byte >= 'A' && byte <= 'F');
}

/* The codepoint of hexadecimal digits (the Specification, "Terminals":
   S07 beyond 10FFFF, S08 for a noncharacter or surrogate). Writes
   SXML_IXML_NONE after a failure. */
static uint32_t read_hex(grammar_compiler *compiler, sxml_byte_view digits) {
    if (digits.byte_count == 0) {
        fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        return SXML_IXML_NONE;
    }
    uint32_t codepoint = 0;
    bool is_too_large = false;
    for (size_t digit_index = 0; digit_index < digits.byte_count; digit_index += 1) {
        const uint8_t byte = digits.bytes[digit_index];
        if (!is_hex_digit(byte)) {
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
            return SXML_IXML_NONE;
        }
        const uint32_t digit = byte <= '9'   ? (uint32_t)(byte - '0')
                               : byte <= 'F' ? (uint32_t)(byte - 'A' + 10)
                                             : (uint32_t)(byte - 'a' + 10);
        if (!is_too_large) {
            codepoint = codepoint * 16 + digit;
            is_too_large = codepoint > 0x10FFFFU;
        }
    }
    if (is_too_large) {
        fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_CODEPOINT_RANGE);
        return SXML_IXML_NONE;
    }
    if ((codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        (codepoint >= 0xFDD0U && codepoint <= 0xFDEFU) || (codepoint & 0xFFFEU) == 0xFFFEU) {
        fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_NONCHARACTER);
        return SXML_IXML_NONE;
    }
    return codepoint;
}

/* One character of a range end: a single character, or "#" and hex
   digits ("-character" of the grammar of grammars). */
static uint32_t read_range_end(grammar_compiler *compiler, sxml_byte_view text) {
    if (text.byte_count > 1 && text.bytes[0] == '#') {
        return read_hex(compiler, (sxml_byte_view){.bytes = text.bytes + 1,
                                                   .byte_count = text.byte_count - 1});
    }
    if (text.byte_count > 0) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(text, 0);
        if (decoded.encoded_byte_count == text.byte_count) {
            return decoded.codepoint;
        }
    }
    fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
    return SXML_IXML_NONE;
}

/* The general categories of a class code (the Specification, "Character
   sets"; UAX #44, 5.7.1), as a mask; 0 for an unknown code. */
static uint32_t class_mask(sxml_byte_view code) {
    static const char category_codes[] = "LuLlLtLmLoMnMcMeNdNlNoPcPdPsPePiPfPoZsZlZpSmScSkSoCcCfCsCoCn";
    if (code.byte_count == 1) {
        switch (code.bytes[0]) {
        case 'L':
            return SXML_REGEX_CATEGORY_MASK_L;
        case 'M':
            return SXML_REGEX_CATEGORY_MASK_M;
        case 'N':
            return SXML_REGEX_CATEGORY_MASK_N;
        case 'P':
            return SXML_REGEX_CATEGORY_MASK_P;
        case 'Z':
            return SXML_REGEX_CATEGORY_MASK_Z;
        case 'S':
            return SXML_REGEX_CATEGORY_MASK_S;
        case 'C':
            return SXML_REGEX_CATEGORY_MASK_C;
        default:
            return 0;
        }
    }
    if (code.byte_count != 2) {
        return 0;
    }
    if (code.bytes[0] == 'L' && code.bytes[1] == 'C') {
        return (1U << SXML_REGEX_GENERAL_CATEGORY_LU) | (1U << SXML_REGEX_GENERAL_CATEGORY_LL) |
               (1U << SXML_REGEX_GENERAL_CATEGORY_LT);
    }
    for (uint32_t category = 0; category <= SXML_REGEX_GENERAL_CATEGORY_CN; category += 1) {
        if (category_codes[(size_t)category * 2] == (char)code.bytes[0] &&
            category_codes[(size_t)category * 2 + 1] == (char)code.bytes[1]) {
            return 1U << category;
        }
    }
    return 0;
}

/* Appends a range to the grammar. */
static sxml_status add_range(grammar_compiler *compiler, sxml_ixml_codepoint_range range) {
    return push_element(&compiler->grammar->ranges, &range, NULL);
}

/* The terminal of one codepoint. */
static sxml_status add_character_terminal(grammar_compiler *compiler, uint32_t codepoint,
                                          uint32_t *terminal_index) {
    const sxml_ixml_terminal terminal = {
        .first_range = (uint32_t)compiler->grammar->ranges.element_count,
        .range_count = 1,
        .category_mask = 0,
        .is_exclusion = false};
    sxml_status status = add_range(compiler, (sxml_ixml_codepoint_range){
                                                 .first_codepoint = codepoint,
                                                 .last_codepoint = codepoint});
    if (status == SXML_STATUS_OK) {
        status = push_element(&compiler->grammar->terminals, &terminal, terminal_index);
    }
    return status;
}

/* The terminal of a character set node (inclusion or exclusion). */
static sxml_status add_set_terminal(grammar_compiler *compiler, uint32_t set_node,
                                    uint32_t *terminal_index) {
    const description_node *set = description_at(compiler->builder, set_node);
    sxml_ixml_terminal terminal = {
        .first_range = (uint32_t)compiler->grammar->ranges.element_count,
        .range_count = 0,
        .category_mask = 0,
        .is_exclusion = set->element_kind == SXML_IXML_ELEMENT_KIND_EXCLUSION};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t member_index = set->first_child;
         status == SXML_STATUS_OK && member_index != SXML_IXML_NONE;
         member_index = description_at(compiler->builder, member_index)->next_sibling) {
        const description_node *member = description_at(compiler->builder, member_index);
        if (member->element_kind != SXML_IXML_ELEMENT_KIND_MEMBER) {
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
            continue;
        }
        if (has_attribute(member, SXML_IXML_ATTRIBUTE_KIND_STRING)) {
            const sxml_byte_view text =
                attribute_value(compiler, member, SXML_IXML_ATTRIBUTE_KIND_STRING);
            if (text.byte_count == 0) {
                fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
            }
            for (size_t byte_offset = 0; status == SXML_STATUS_OK && byte_offset < text.byte_count;) {
                const sxml_xml_decoded_codepoint decoded =
                    sxml_xml_decode_codepoint(text, byte_offset);
                byte_offset += decoded.encoded_byte_count;
                status = add_range(compiler, (sxml_ixml_codepoint_range){
                                                 .first_codepoint = decoded.codepoint,
                                                 .last_codepoint = decoded.codepoint});
                terminal.range_count += 1;
            }
        } else if (has_attribute(member, SXML_IXML_ATTRIBUTE_KIND_HEX)) {
            const uint32_t codepoint = read_hex(
                compiler, attribute_value(compiler, member, SXML_IXML_ATTRIBUTE_KIND_HEX));
            status = add_range(compiler, (sxml_ixml_codepoint_range){.first_codepoint = codepoint,
                                                                     .last_codepoint = codepoint});
            terminal.range_count += 1;
        } else if (has_attribute(member, SXML_IXML_ATTRIBUTE_KIND_FROM) &&
                   has_attribute(member, SXML_IXML_ATTRIBUTE_KIND_TO)) {
            const uint32_t first = read_range_end(
                compiler, attribute_value(compiler, member, SXML_IXML_ATTRIBUTE_KIND_FROM));
            const uint32_t last = read_range_end(
                compiler, attribute_value(compiler, member, SXML_IXML_ATTRIBUTE_KIND_TO));
            if (first != SXML_IXML_NONE && last != SXML_IXML_NONE && first > last) {
                fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_RANGE_ORDER);
            }
            status = add_range(compiler, (sxml_ixml_codepoint_range){.first_codepoint = first,
                                                                     .last_codepoint = last});
            terminal.range_count += 1;
        } else if (has_attribute(member, SXML_IXML_ATTRIBUTE_KIND_CODE)) {
            const uint32_t mask =
                class_mask(attribute_value(compiler, member, SXML_IXML_ATTRIBUTE_KIND_CODE));
            if (mask == 0) {
                fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_UNKNOWN_CLASS);
            }
            terminal.category_mask |= mask;
        } else {
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = push_element(&compiler->grammar->terminals, &terminal, terminal_index);
    }
    return status;
}

static int compare_rule_names(const void *left, const void *right) {
    const rule_name *left_name = left;
    const rule_name *right_name = right;
    const size_t common = left_name->name.byte_count < right_name->name.byte_count
                              ? left_name->name.byte_count
                              : right_name->name.byte_count;
    const int order = common == 0 ? 0 : memcmp(left_name->name.bytes, right_name->name.bytes, common);
    if (order != 0) {
        return order;
    }
    if (left_name->name.byte_count != right_name->name.byte_count) {
        return left_name->name.byte_count < right_name->name.byte_count ? -1 : 1;
    }
    return 0;
}

/* The rule nonterminal named name, or SXML_IXML_NONE. */
static uint32_t find_rule(const grammar_compiler *compiler, sxml_byte_view name) {
    size_t low = 0;
    size_t high = compiler->rule_count;
    const rule_name key = {.name = name, .nonterminal = 0};
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const int order = compare_rule_names(&compiler->rule_names[middle], &key);
        if (order == 0) {
            return compiler->rule_names[middle].nonterminal;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return SXML_IXML_NONE;
}

/* A hidden nonterminal the compiler makes for node, compiled later as
   kind. */
static sxml_status add_generated(grammar_compiler *compiler, const pending_nonterminal *request,
                                 uint32_t *nonterminal_index) {
    const sxml_ixml_nonterminal nonterminal = {
        .name = {.byte_offset = 0, .byte_count = 0},
        .alias = {.byte_offset = 0, .byte_count = 0},
        .mark = SXML_IXML_MARK_HIDDEN,
        .is_generated = true,
        .first_production = 0,
        .production_count = 0};
    sxml_status status =
        push_element(&compiler->grammar->nonterminals, &nonterminal, nonterminal_index);
    if (status == SXML_STATUS_OK) {
        const pending_nonterminal pending = {
            .nonterminal = *nonterminal_index, .node = request->node, .kind = request->kind};
        status = push_element(&compiler->pending, &pending, NULL);
    }
    return status;
}

/* Appends a symbol to the grammar. */
static sxml_status add_symbol(grammar_compiler *compiler, const sxml_ixml_symbol *symbol) {
    return push_element(&compiler->grammar->symbols, symbol, NULL);
}

/* A use of the hidden nonterminal the compiler makes for node. */
static sxml_status add_generated_symbol(grammar_compiler *compiler, uint32_t node,
                                        sxml_ixml_construct_kind kind) {
    uint32_t nonterminal = 0;
    sxml_status status = add_generated(
        compiler, &(pending_nonterminal){.nonterminal = 0, .node = node, .kind = kind},
        &nonterminal);
    if (status == SXML_STATUS_OK) {
        status = add_symbol(compiler, &(sxml_ixml_symbol){
                                          .symbol_kind = SXML_IXML_SYMBOL_KIND_NONTERMINAL,
                                          .mark = SXML_IXML_MARK_HIDDEN,
                                          .target = nonterminal,
                                          .text = {.byte_offset = 0, .byte_count = 0}});
    }
    return status;
}

/* Appends the symbols of one factor node to the production being built. */
static sxml_status add_factor(grammar_compiler *compiler, uint32_t node_index) {
    const description_node *node = description_at(compiler->builder, node_index);
    sxml_status status = SXML_STATUS_OK;
    switch ((sxml_ixml_element_kind)node->element_kind) {
    case SXML_IXML_ELEMENT_KIND_NONTERMINAL: {
        const uint32_t target =
            find_rule(compiler, attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_NAME));
        if (target == SXML_IXML_NONE) {
            if (compiler->error == SXML_IXML_GRAMMAR_ERROR_NONE) {
                compiler->error_name =
                    attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_NAME);
            }
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_UNDEFINED_NONTERMINAL);
            return SXML_STATUS_OK;
        }
        sxml_ixml_symbol symbol = {
            .symbol_kind = SXML_IXML_SYMBOL_KIND_NONTERMINAL,
            .mark = (uint8_t)read_nonterminal_mark(
                compiler,
                (mark_request){
                    .text = attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_MARK)}),
            .target = target,
            .text = {.byte_offset = 0, .byte_count = 0}};
        status = store_text(compiler,
                            attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_ALIAS),
                            &symbol.text);
        if (status == SXML_STATUS_OK) {
            status = add_symbol(compiler, &symbol);
        }
        return status;
    }
    case SXML_IXML_ELEMENT_KIND_LITERAL: {
        const uint8_t mark = (uint8_t)read_terminal_mark(
            compiler,
            (mark_request){.text = attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_TMARK)});
        if (has_attribute(node, SXML_IXML_ATTRIBUTE_KIND_HEX)) {
            const uint32_t codepoint =
                read_hex(compiler, attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_HEX));
            uint32_t terminal = 0;
            status = add_character_terminal(compiler, codepoint, &terminal);
            if (status == SXML_STATUS_OK) {
                status = add_symbol(compiler, &(sxml_ixml_symbol){
                                                  .symbol_kind = SXML_IXML_SYMBOL_KIND_TERMINAL,
                                                  .mark = mark,
                                                  .target = terminal,
                                                  .text = {.byte_offset = 0, .byte_count = 0}});
            }
            return status;
        }
        const sxml_byte_view text = attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_STRING);
        if (text.byte_count == 0) {
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        }
        for (size_t byte_offset = 0; status == SXML_STATUS_OK && byte_offset < text.byte_count;) {
            const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(text, byte_offset);
            byte_offset += decoded.encoded_byte_count;
            uint32_t terminal = 0;
            status = add_character_terminal(compiler, decoded.codepoint, &terminal);
            if (status == SXML_STATUS_OK) {
                status = add_symbol(compiler, &(sxml_ixml_symbol){
                                                  .symbol_kind = SXML_IXML_SYMBOL_KIND_TERMINAL,
                                                  .mark = mark,
                                                  .target = terminal,
                                                  .text = {.byte_offset = 0, .byte_count = 0}});
            }
        }
        return status;
    }
    case SXML_IXML_ELEMENT_KIND_INCLUSION:
    case SXML_IXML_ELEMENT_KIND_EXCLUSION: {
        const uint8_t mark = (uint8_t)read_terminal_mark(
            compiler,
            (mark_request){.text = attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_TMARK)});
        uint32_t terminal = 0;
        status = add_set_terminal(compiler, node_index, &terminal);
        if (status == SXML_STATUS_OK) {
            status = add_symbol(compiler, &(sxml_ixml_symbol){
                                              .symbol_kind = SXML_IXML_SYMBOL_KIND_TERMINAL,
                                              .mark = mark,
                                              .target = terminal,
                                              .text = {.byte_offset = 0, .byte_count = 0}});
        }
        return status;
    }
    case SXML_IXML_ELEMENT_KIND_INSERTION: {
        sxml_ixml_symbol symbol = {.symbol_kind = SXML_IXML_SYMBOL_KIND_INSERTION,
                                   .mark = SXML_IXML_MARK_NONE,
                                   .target = 0,
                                   .text = {.byte_offset = 0, .byte_count = 0}};
        if (has_attribute(node, SXML_IXML_ATTRIBUTE_KIND_HEX)) {
            const uint32_t codepoint =
                read_hex(compiler, attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_HEX));
            uint8_t encoded[4];
            sxml_xml_buffer scratch = sxml_xml_buffer_empty(compiler->builder->allocator);
            if (codepoint != SXML_IXML_NONE) {
                status = sxml_xml_buffer_append_codepoint(&scratch, codepoint);
            }
            const size_t byte_count = scratch.byte_count;
            if (byte_count > 0) {
                memcpy(encoded, scratch.bytes, byte_count);
            }
            sxml_xml_buffer_release(&scratch);
            if (status == SXML_STATUS_OK) {
                status = store_text(compiler,
                                    (sxml_byte_view){.bytes = encoded, .byte_count = byte_count},
                                    &symbol.text);
            }
        } else {
            const sxml_byte_view text =
                attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_STRING);
            if (text.byte_count == 0) {
                fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
            }
            status = store_text(compiler, text, &symbol.text);
        }
        if (status == SXML_STATUS_OK) {
            status = add_symbol(compiler, &symbol);
        }
        return status;
    }
    case SXML_IXML_ELEMENT_KIND_ALTS:
        return add_generated_symbol(compiler, node_index, SXML_IXML_CONSTRUCT_KIND_ALTERNATIVES);
    case SXML_IXML_ELEMENT_KIND_OPTION:
        return add_generated_symbol(compiler, node_index, SXML_IXML_CONSTRUCT_KIND_OPTION);
    case SXML_IXML_ELEMENT_KIND_REPEAT0:
        return add_generated_symbol(compiler, node_index, SXML_IXML_CONSTRUCT_KIND_REPEAT0);
    case SXML_IXML_ELEMENT_KIND_REPEAT1:
        return add_generated_symbol(compiler, node_index, SXML_IXML_CONSTRUCT_KIND_REPEAT1);
    case SXML_IXML_ELEMENT_KIND_UNKNOWN:
    case SXML_IXML_ELEMENT_KIND_IXML:
    case SXML_IXML_ELEMENT_KIND_PROLOG:
    case SXML_IXML_ELEMENT_KIND_VERSION:
    case SXML_IXML_ELEMENT_KIND_RULE:
    case SXML_IXML_ELEMENT_KIND_ALT:
    case SXML_IXML_ELEMENT_KIND_MEMBER:
    case SXML_IXML_ELEMENT_KIND_SEP:
    case SXML_IXML_ELEMENT_KIND_COMMENT:
    case SXML_IXML_ELEMENT_KIND_COUNT:
        break;
    }
    fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
    return SXML_STATUS_OK;
}

/* A production being built: its nonterminal and where its symbols
   start. */
typedef struct production_start {
    uint32_t nonterminal;
    uint32_t first_symbol;
} production_start;

static production_start start_production(const grammar_compiler *compiler, uint32_t nonterminal) {
    return (production_start){.nonterminal = nonterminal,
                              .first_symbol = (uint32_t)compiler->grammar->symbols.element_count};
}

static sxml_status end_production(grammar_compiler *compiler, production_start start) {
    const sxml_ixml_production production = {
        .nonterminal = start.nonterminal,
        .first_symbol = start.first_symbol,
        .symbol_count = (uint32_t)compiler->grammar->symbols.element_count - start.first_symbol};
    return push_element(&compiler->productions, &production, NULL);
}

/* The use of nonterminal itself, hidden (the left recursion of a
   repetition). */
static sxml_status add_self(grammar_compiler *compiler, uint32_t nonterminal) {
    return add_symbol(compiler, &(sxml_ixml_symbol){
                                    .symbol_kind = SXML_IXML_SYMBOL_KIND_NONTERMINAL,
                                    .mark = SXML_IXML_MARK_HIDDEN,
                                    .target = nonterminal,
                                    .text = {.byte_offset = 0, .byte_count = 0}});
}

/* The factor and separator of a repetition or option node. */
typedef struct repetition_parts {
    uint32_t factor;
    uint32_t separator;
} repetition_parts;

static repetition_parts read_repetition(grammar_compiler *compiler, uint32_t node_index) {
    const description_node *node = description_at(compiler->builder, node_index);
    repetition_parts parts = {.factor = node->first_child, .separator = SXML_IXML_NONE};
    if (parts.factor == SXML_IXML_NONE) {
        fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        return parts;
    }
    const uint32_t next = description_at(compiler->builder, parts.factor)->next_sibling;
    if (next != SXML_IXML_NONE) {
        const description_node *separator = description_at(compiler->builder, next);
        if (node->element_kind == SXML_IXML_ELEMENT_KIND_OPTION ||
            separator->element_kind != SXML_IXML_ELEMENT_KIND_SEP ||
            separator->first_child == SXML_IXML_NONE || separator->next_sibling != SXML_IXML_NONE ||
            description_at(compiler->builder, separator->first_child)->next_sibling !=
                SXML_IXML_NONE) {
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        } else {
            parts.separator = separator->first_child;
        }
    }
    return parts;
}

/* Compiles the productions of one pending nonterminal. */
static sxml_status compile_pending(grammar_compiler *compiler, pending_nonterminal pending) {
    const uint32_t nonterminal = pending.nonterminal;
    sxml_status status = SXML_STATUS_OK;
    if (pending.kind == SXML_IXML_CONSTRUCT_KIND_ALTERNATIVES) {
        const description_node *node = description_at(compiler->builder, pending.node);
        bool has_alternative = false;
        for (uint32_t alternative = node->first_child;
             status == SXML_STATUS_OK && alternative != SXML_IXML_NONE;
             alternative = description_at(compiler->builder, alternative)->next_sibling) {
            const description_node *alternative_node = description_at(compiler->builder, alternative);
            if (alternative_node->element_kind != SXML_IXML_ELEMENT_KIND_ALT) {
                fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
                continue;
            }
            has_alternative = true;
            const production_start start = start_production(compiler, nonterminal);
            for (uint32_t term_node = alternative_node->first_child;
                 status == SXML_STATUS_OK && term_node != SXML_IXML_NONE;
                 term_node = description_at(compiler->builder, term_node)->next_sibling) {
                status = add_factor(compiler, term_node);
            }
            if (status == SXML_STATUS_OK) {
                status = end_production(compiler, start);
            }
        }
        if (!has_alternative) {
            /* A rule has one or more alternatives (the Specification,
               "Rules"). */
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        }
        return status;
    }
    const repetition_parts parts = read_repetition(compiler, pending.node);
    if (parts.factor == SXML_IXML_NONE) {
        return SXML_STATUS_OK;
    }
    if (pending.kind == SXML_IXML_CONSTRUCT_KIND_OPTION ||
        (pending.kind == SXML_IXML_CONSTRUCT_KIND_REPEAT0 && parts.separator != SXML_IXML_NONE)) {
        /* f? : () | f; f**sep : () | f++sep. */
        status = end_production(compiler, start_production(compiler, nonterminal));
        const production_start start = start_production(compiler, nonterminal);
        if (status == SXML_STATUS_OK && pending.kind == SXML_IXML_CONSTRUCT_KIND_OPTION) {
            status = add_factor(compiler, parts.factor);
        } else if (status == SXML_STATUS_OK) {
            status = add_generated_symbol(compiler, pending.node, SXML_IXML_CONSTRUCT_KIND_REPEAT1);
        }
        if (status == SXML_STATUS_OK) {
            status = end_production(compiler, start);
        }
        return status;
    }
    /* f* : () | G, f; f+ : f | G, f; f++sep : f | G, sep, f. */
    production_start start = start_production(compiler, nonterminal);
    if (pending.kind == SXML_IXML_CONSTRUCT_KIND_REPEAT1) {
        status = add_factor(compiler, parts.factor);
    }
    if (status == SXML_STATUS_OK) {
        status = end_production(compiler, start);
    }
    start = start_production(compiler, nonterminal);
    if (status == SXML_STATUS_OK) {
        status = add_self(compiler, nonterminal);
    }
    if (status == SXML_STATUS_OK && parts.separator != SXML_IXML_NONE) {
        status = add_factor(compiler, parts.separator);
    }
    if (status == SXML_STATUS_OK) {
        status = add_factor(compiler, parts.factor);
    }
    if (status == SXML_STATUS_OK) {
        status = end_production(compiler, start);
    }
    return status;
}

/* Reads the prolog (the Specification, "Prolog"): sxml recognizes "1.0"
   and "1.1", the version of the editorial draft with renaming, and uses
   the editorial draft's syntax and semantics for every grammar, so any
   other version is a mismatch (D215). */
static void read_prolog(grammar_compiler *compiler, uint32_t prolog_index) {
    const description_node *prolog = description_at(compiler->builder, prolog_index);
    const uint32_t version_index = prolog->first_child;
    if (version_index == SXML_IXML_NONE ||
        description_at(compiler->builder, version_index)->next_sibling != SXML_IXML_NONE) {
        fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        return;
    }
    const description_node *version = description_at(compiler->builder, version_index);
    if (version->element_kind != SXML_IXML_ELEMENT_KIND_VERSION ||
        !has_attribute(version, SXML_IXML_ATTRIBUTE_KIND_STRING)) {
        fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        return;
    }
    const sxml_byte_view declared =
        attribute_value(compiler, version, SXML_IXML_ATTRIBUTE_KIND_STRING);
    compiler->grammar->has_version_mismatch =
        !sxml_byte_view_equals(declared, SXML_BYTE_VIEW_FROM_LITERAL("1.0")) &&
        !sxml_byte_view_equals(declared, SXML_BYTE_VIEW_FROM_LITERAL("1.1"));
}

/* Makes one nonterminal per rule, in order, and the sorted name table. */
static sxml_status add_rules(grammar_compiler *compiler) {
    const description_node *root = description_at(compiler->builder, 0);
    size_t rule_count = 0;
    for (uint32_t child = root->first_child; child != SXML_IXML_NONE;
         child = description_at(compiler->builder, child)->next_sibling) {
        const description_node *node = description_at(compiler->builder, child);
        if (node->element_kind == SXML_IXML_ELEMENT_KIND_RULE) {
            rule_count += 1;
        } else if (node->element_kind == SXML_IXML_ELEMENT_KIND_PROLOG && rule_count == 0 &&
                   child == root->first_child) {
            read_prolog(compiler, child);
        } else {
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        }
    }
    if (rule_count == 0) {
        fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        compiler->builder->allocator,
        (sxml_array_allocation_request){.element_count = rule_count,
                                        .element_size = sizeof(rule_name),
                                        .element_alignment = _Alignof(rule_name)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    compiler->rule_names = memory;
    for (uint32_t child = root->first_child; status == SXML_STATUS_OK && child != SXML_IXML_NONE;
         child = description_at(compiler->builder, child)->next_sibling) {
        const description_node *node = description_at(compiler->builder, child);
        if (node->element_kind != SXML_IXML_ELEMENT_KIND_RULE) {
            continue;
        }
        const sxml_byte_view name = attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_NAME);
        if (name.byte_count == 0) {
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
        }
        sxml_ixml_nonterminal nonterminal = {
            .mark = (uint8_t)read_nonterminal_mark(
                compiler,
                (mark_request){
                    .text = attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_MARK)}),
            .is_generated = false,
            .first_production = 0,
            .production_count = 0};
        status = store_text(compiler, name, &nonterminal.name);
        if (status == SXML_STATUS_OK) {
            status = store_text(compiler,
                                attribute_value(compiler, node, SXML_IXML_ATTRIBUTE_KIND_ALIAS),
                                &nonterminal.alias);
        }
        uint32_t nonterminal_index = 0;
        if (status == SXML_STATUS_OK) {
            status = push_element(&compiler->grammar->nonterminals, &nonterminal, &nonterminal_index);
        }
        if (status == SXML_STATUS_OK) {
            compiler->rule_names[compiler->rule_count] =
                (rule_name){.name = name, .nonterminal = nonterminal_index};
            compiler->rule_count += 1;
            const pending_nonterminal pending = {.nonterminal = nonterminal_index,
                                                 .node = child,
                                                 .kind = SXML_IXML_CONSTRUCT_KIND_ALTERNATIVES};
            status = push_element(&compiler->pending, &pending, NULL);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    qsort(compiler->rule_names, compiler->rule_count, sizeof(rule_name), compare_rule_names);
    for (size_t rule_index = 1; rule_index < compiler->rule_count; rule_index += 1) {
        if (compare_rule_names(&compiler->rule_names[rule_index - 1],
                               &compiler->rule_names[rule_index]) == 0) {
            if (compiler->error == SXML_IXML_GRAMMAR_ERROR_NONE) {
                compiler->error_name = compiler->rule_names[rule_index].name;
            }
            fail_compiling(compiler, SXML_IXML_GRAMMAR_ERROR_DUPLICATE_RULE);
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Simplifying */

static sxml_xml_array empty_array(sxml_allocator allocator, size_t element_byte_count) {
    return sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = allocator, .element_byte_count = element_byte_count});
}


/* The most productions one production may become when the groups and
   options in it are written out in place (D283). */
#define SXML_IXML_MAXIMUM_WRITTEN_OUT 8U

/* The mark of a use of a nonterminal: the use's, else its rule's, else an
   element (the Specification, "Constructing XML"). */
static sxml_ixml_mark use_mark(const grammar_compiler *compiler, const sxml_ixml_symbol *symbol) {
    sxml_ixml_mark mark = (sxml_ixml_mark)symbol->mark;
    if (mark == SXML_IXML_MARK_NONE) {
        const sxml_ixml_nonterminal *target =
            sxml_xml_array_at(&compiler->grammar->nonterminals, symbol->target);
        mark = (sxml_ixml_mark)target->mark;
    }
    return mark == SXML_IXML_MARK_NONE ? SXML_IXML_MARK_ELEMENT : mark;
}

/* Where the productions of each nonterminal are in the compiler's list:
   compile_pending adds those of one nonterminal together. */
typedef struct production_runs {
    uint32_t *first_productions;
    uint32_t *production_counts;
    /* Per nonterminal: the productions it becomes when written out where
       it is used; 0 when it is not written out. */
    uint32_t *written_counts;
} production_runs;

static const sxml_ixml_production *listed_production(const grammar_compiler *compiler,
                                                    uint32_t production_index) {
    return sxml_xml_array_at(&compiler->productions, production_index);
}

static const sxml_ixml_symbol *grammar_symbol_at(const grammar_compiler *compiler,
                                                 uint32_t symbol_index) {
    return sxml_xml_array_at(&compiler->grammar->symbols, symbol_index);
}

/* A hidden use of a rule whose one production is one symbol uses that
   symbol in its place: a rule such as -digit: ["0"-"9"] or -s: " "*
   makes no element of its own, so the XML is the same, and the parser
   makes two items fewer each time it reads one (D283). One pass, so a
   rule that is its own one symbol stays as it is. */
static void write_out_single_rules(grammar_compiler *compiler, const production_runs *runs) {
    const size_t nonterminal_count = compiler->grammar->nonterminals.element_count;
    for (size_t symbol_index = 0; symbol_index < compiler->grammar->symbols.element_count;
         symbol_index += 1) {
        sxml_ixml_symbol *symbol = sxml_xml_array_at(&compiler->grammar->symbols, symbol_index);
        if (symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_NONTERMINAL ||
            symbol->target >= nonterminal_count || symbol->target == 0 ||
            runs->production_counts[symbol->target] != 1 ||
            use_mark(compiler, symbol) != SXML_IXML_MARK_HIDDEN) {
            continue;
        }
        const sxml_ixml_production *production =
            listed_production(compiler, runs->first_productions[symbol->target]);
        if (production->symbol_count == 1) {
            *symbol = *grammar_symbol_at(compiler, production->first_symbol);
        }
    }
}

/* Decides which groups and options are written out where they are used:
   a group or option the compiler made is used once, hidden, so its
   productions can replace the production that uses it, one for each,
   with the same parses and the same XML. Children are made after their
   parents, so the counts are found from the last nonterminal back. A
   production may become at most SXML_IXML_MAXIMUM_WRITTEN_OUT. */
static void choose_written_out(grammar_compiler *compiler, production_runs *runs,
                               const uint8_t *kinds) {
    const size_t nonterminal_count = compiler->grammar->nonterminals.element_count;
    for (size_t nonterminal_index = nonterminal_count; nonterminal_index > 0;
         nonterminal_index -= 1) {
        const uint32_t nonterminal = (uint32_t)nonterminal_index - 1;
        const sxml_ixml_nonterminal *definition =
            sxml_xml_array_at(&compiler->grammar->nonterminals, nonterminal);
        uint64_t total = 0;
        for (uint32_t production_offset = 0; production_offset < runs->production_counts[nonterminal];
             production_offset += 1) {
            const sxml_ixml_production *production = listed_production(
                compiler, runs->first_productions[nonterminal] + production_offset);
            uint64_t product = 1;
            for (uint32_t dot_position = 0; dot_position < production->symbol_count;
                 dot_position += 1) {
                const sxml_ixml_symbol *symbol =
                    grammar_symbol_at(compiler, production->first_symbol + dot_position);
                if (symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_NONTERMINAL ||
                    runs->written_counts[symbol->target] == 0) {
                    continue;
                }
                if (product * runs->written_counts[symbol->target] > SXML_IXML_MAXIMUM_WRITTEN_OUT) {
                    runs->written_counts[symbol->target] = 0;
                    continue;
                }
                product *= runs->written_counts[symbol->target];
            }
            total += product;
        }
        const bool is_candidate =
            definition->is_generated &&
            (kinds[nonterminal] == SXML_IXML_CONSTRUCT_KIND_ALTERNATIVES ||
             kinds[nonterminal] == SXML_IXML_CONSTRUCT_KIND_OPTION);
        runs->written_counts[nonterminal] =
            is_candidate && total > 0 && total <= SXML_IXML_MAXIMUM_WRITTEN_OUT ? (uint32_t)total : 0;
    }
}

/* A run of symbols in the grammar: a production being written out. */
typedef struct symbol_run {
    uint32_t first_symbol;
    uint32_t symbol_count;
} symbol_run;

/* Appends to the grammar's symbols the run with the symbol at
   replaced_offset replaced by the symbols of production; *written is the
   new run. */
typedef struct splice {
    symbol_run run;
    uint32_t replaced_offset;
    const sxml_ixml_production *production;
} splice;

static sxml_status write_splice(grammar_compiler *compiler, const splice *request, symbol_run *written) {
    sxml_xml_array *symbols = &compiler->grammar->symbols;
    written->first_symbol = (uint32_t)symbols->element_count;
    written->symbol_count = request->run.symbol_count - 1 + request->production->symbol_count;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t offset = 0; status == SXML_STATUS_OK && offset < request->run.symbol_count;
         offset += 1) {
        if (offset != request->replaced_offset) {
            const sxml_ixml_symbol copy = *grammar_symbol_at(compiler, request->run.first_symbol + offset);
            status = push_element(symbols, &copy, NULL);
            continue;
        }
        for (uint32_t inner = 0; status == SXML_STATUS_OK && inner < request->production->symbol_count;
             inner += 1) {
            const sxml_ixml_symbol copy =
                *grammar_symbol_at(compiler, request->production->first_symbol + inner);
            status = push_element(symbols, &copy, NULL);
        }
    }
    return status;
}

/* Writes one production out into output: depth first, the alternatives
   of each written-out group in order, so the order of alternatives is
   kept. */
typedef struct write_out_request {
    const production_runs *runs;
    const sxml_ixml_production *production;
    sxml_xml_array *output;
    sxml_xml_array *stack; /* symbol_run */
} write_out_request;

static sxml_status write_out_production(grammar_compiler *compiler, const write_out_request *request) {
    const uint32_t nonterminal = request->production->nonterminal;
    request->stack->element_count = 0;
    const symbol_run start = {.first_symbol = request->production->first_symbol,
                              .symbol_count = request->production->symbol_count};
    sxml_status status = push_element(request->stack, &start, NULL);
    while (status == SXML_STATUS_OK && request->stack->element_count > 0) {
        const symbol_run run =
            *(const symbol_run *)sxml_xml_array_at(request->stack, request->stack->element_count - 1);
        request->stack->element_count -= 1;
        uint32_t replaced_offset = SXML_IXML_NONE;
        for (uint32_t offset = 0; replaced_offset == SXML_IXML_NONE && offset < run.symbol_count;
             offset += 1) {
            const sxml_ixml_symbol *symbol = grammar_symbol_at(compiler, run.first_symbol + offset);
            if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_NONTERMINAL &&
                request->runs->written_counts[symbol->target] != 0) {
                replaced_offset = offset;
            }
        }
        if (replaced_offset == SXML_IXML_NONE) {
            const sxml_ixml_production written = {
                .nonterminal = nonterminal, .first_symbol = run.first_symbol, .symbol_count = run.symbol_count};
            status = push_element(request->output, &written, NULL);
            continue;
        }
        const uint32_t group = grammar_symbol_at(compiler, run.first_symbol + replaced_offset)->target;
        for (uint32_t alternative = request->runs->production_counts[group];
             status == SXML_STATUS_OK && alternative > 0; alternative -= 1) {
            const sxml_ixml_production inner = *listed_production(
                compiler, request->runs->first_productions[group] + alternative - 1);
            symbol_run written = {.first_symbol = 0, .symbol_count = 0};
            status = write_splice(compiler,
                                  &(splice){.run = run, .replaced_offset = replaced_offset, .production = &inner},
                                  &written);
            if (status == SXML_STATUS_OK) {
                status = push_element(request->stack, &written, NULL);
            }
        }
    }
    return status;
}

/* Simplifies the compiled productions without changing the parses or
   the XML (D283): hidden rules of one symbol are read in place, and
   groups and options are written out where they are used, so the parser
   makes fewer items for each character. */
static sxml_status simplify_productions(grammar_compiler *compiler) {
    const sxml_allocator allocator = compiler->builder->allocator;
    const size_t nonterminal_count = compiler->grammar->nonterminals.element_count;
    production_runs runs = {.first_productions = NULL, .production_counts = NULL, .written_counts = NULL};
    uint8_t *kinds = NULL;
    void *memory = NULL;
    sxml_array_allocation_request index_request = {
        .element_count = nonterminal_count, .element_size = sizeof(uint32_t), .element_alignment = _Alignof(uint32_t)};
    sxml_status status = sxml_allocator_allocate_array(allocator, index_request, &memory);
    runs.first_productions = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(allocator, index_request, &memory);
        runs.production_counts = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(allocator, index_request, &memory);
        runs.written_counts = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){
                .element_count = nonterminal_count, .element_size = 1, .element_alignment = 1},
            &memory);
        kinds = memory;
    }
    sxml_xml_array output = empty_array(allocator, sizeof(sxml_ixml_production));
    sxml_xml_array stack = empty_array(allocator, sizeof(symbol_run));
    if (status == SXML_STATUS_OK) {
        for (size_t pending_index = 0; pending_index < compiler->pending.element_count; pending_index += 1) {
            const pending_nonterminal *pending = sxml_xml_array_at(&compiler->pending, pending_index);
            kinds[pending->nonterminal] = (uint8_t)pending->kind;
        }
        for (uint32_t production_index = (uint32_t)compiler->productions.element_count; production_index > 0;
             production_index -= 1) {
            const uint32_t nonterminal = listed_production(compiler, production_index - 1)->nonterminal;
            runs.first_productions[nonterminal] = production_index - 1;
            runs.production_counts[nonterminal] += 1;
        }
        write_out_single_rules(compiler, &runs);
        choose_written_out(compiler, &runs, kinds);
    }
    for (uint32_t production_index = 0;
         status == SXML_STATUS_OK && production_index < compiler->productions.element_count;
         production_index += 1) {
        const sxml_ixml_production production = *listed_production(compiler, production_index);
        if (runs.written_counts[production.nonterminal] != 0) {
            /* Written out where the group is used. */
            continue;
        }
        status = write_out_production(compiler, &(write_out_request){.runs = &runs,
                                                                    .production = &production,
                                                                    .output = &output,
                                                                    .stack = &stack});
    }
    if (status == SXML_STATUS_OK) {
        sxml_xml_array_release(&compiler->productions);
        compiler->productions = output;
        output = empty_array(allocator, sizeof(sxml_ixml_production));
    }
    sxml_xml_array_release(&output);
    sxml_xml_array_release(&stack);
    sxml_allocator_release(allocator, kinds);
    sxml_allocator_release(allocator, runs.written_counts);
    sxml_allocator_release(allocator, runs.production_counts);
    sxml_allocator_release(allocator, runs.first_productions);
    return status;
}

/* Orders the productions by nonterminal (keeping their order within
   one) and records each nonterminal's. */
static sxml_status sort_productions(grammar_compiler *compiler) {
    sxml_ixml_grammar *grammar = compiler->grammar;
    const size_t production_count = compiler->productions.element_count;
    for (size_t production_index = 0; production_index < production_count; production_index += 1) {
        const sxml_ixml_production *production =
            sxml_xml_array_at(&compiler->productions, production_index);
        sxml_ixml_nonterminal *nonterminal =
            sxml_xml_array_at(&grammar->nonterminals, production->nonterminal);
        nonterminal->production_count += 1;
    }
    uint32_t next_production = 0;
    for (size_t nonterminal_index = 0; nonterminal_index < grammar->nonterminals.element_count;
         nonterminal_index += 1) {
        sxml_ixml_nonterminal *nonterminal =
            sxml_xml_array_at(&grammar->nonterminals, nonterminal_index);
        nonterminal->first_production = next_production;
        next_production += nonterminal->production_count;
        /* Counted again while placing. */
        nonterminal->production_count = 0;
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t production_index = 0; status == SXML_STATUS_OK && production_index < production_count;
         production_index += 1) {
        void *element = NULL;
        status = sxml_xml_array_push(&grammar->productions, &element);
    }
    for (size_t production_index = 0; status == SXML_STATUS_OK && production_index < production_count;
         production_index += 1) {
        const sxml_ixml_production *production =
            sxml_xml_array_at(&compiler->productions, production_index);
        sxml_ixml_nonterminal *nonterminal =
            sxml_xml_array_at(&grammar->nonterminals, production->nonterminal);
        sxml_ixml_production *placed = sxml_xml_array_at(
            &grammar->productions, nonterminal->first_production + nonterminal->production_count);
        *placed = *production;
        nonterminal->production_count += 1;
    }
    return status;
}

sxml_status sxml_ixml_grammar_builder_finish(sxml_ixml_grammar_builder *builder,
                                             sxml_ixml_grammar *grammar,
                                             sxml_ixml_grammar_outcome *outcome) {
    const sxml_allocator allocator = builder->allocator;
    *grammar = (sxml_ixml_grammar){
        .text = sxml_xml_buffer_empty(allocator),
        .nonterminals = empty_array(allocator, sizeof(sxml_ixml_nonterminal)),
        .productions = empty_array(allocator, sizeof(sxml_ixml_production)),
        .symbols = empty_array(allocator, sizeof(sxml_ixml_symbol)),
        .terminals = empty_array(allocator, sizeof(sxml_ixml_terminal)),
        .ranges = empty_array(allocator, sizeof(sxml_ixml_codepoint_range)),
        .has_version_mismatch = false};
    grammar_compiler compiler = {.builder = builder,
                                 .grammar = grammar,
                                 .pending = empty_array(allocator, sizeof(pending_nonterminal)),
                                 .productions = empty_array(allocator, sizeof(sxml_ixml_production)),
                                 .rule_names = NULL,
                                 .rule_count = 0,
                                 .error = builder->error,
                                 .error_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = SXML_STATUS_OK;
    if (builder->nodes.element_count == 0 ||
        description_at(builder, 0)->element_kind != SXML_IXML_ELEMENT_KIND_IXML) {
        fail_compiling(&compiler, SXML_IXML_GRAMMAR_ERROR_FORM);
    } else {
        status = add_rules(&compiler);
    }
    /* Pending nonterminals are compiled in the order they were made;
       compiling one may add more. */
    for (size_t pending_index = 0; status == SXML_STATUS_OK &&
                                   compiler.error == SXML_IXML_GRAMMAR_ERROR_NONE &&
                                   pending_index < compiler.pending.element_count;
         pending_index += 1) {
        const pending_nonterminal pending =
            *(const pending_nonterminal *)sxml_xml_array_at(&compiler.pending, pending_index);
        status = compile_pending(&compiler, pending);
    }
    if (status == SXML_STATUS_OK && compiler.error == SXML_IXML_GRAMMAR_ERROR_NONE) {
        status = simplify_productions(&compiler);
    }
    if (status == SXML_STATUS_OK && compiler.error == SXML_IXML_GRAMMAR_ERROR_NONE) {
        status = sort_productions(&compiler);
    }
    if (status == SXML_STATUS_OK && compiler.error == SXML_IXML_GRAMMAR_ERROR_NONE) {
        status = sxml_ixml_grammar_prepare_tables(grammar, allocator);
    }
    sxml_allocator_release(allocator, compiler.rule_names);
    sxml_xml_array_release(&compiler.pending);
    sxml_xml_array_release(&compiler.productions);
    outcome->error = status == SXML_STATUS_OK ? compiler.error : SXML_IXML_GRAMMAR_ERROR_NONE;
    outcome->name = outcome->error == SXML_IXML_GRAMMAR_ERROR_UNDEFINED_NONTERMINAL ||
                            outcome->error == SXML_IXML_GRAMMAR_ERROR_DUPLICATE_RULE
                        ? compiler.error_name
                        : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    if (status != SXML_STATUS_OK || compiler.error != SXML_IXML_GRAMMAR_ERROR_NONE) {
        sxml_ixml_grammar_finalize(grammar);
    }
    return status;
}

/* ------------------------------------------------- Constructing XML */

typedef struct xml_emitter {
    const sxml_ixml_construction_request *request;
    const sxml_ixml_grammar *grammar;
    sxml_tree_builder builder;
    sxml_xml_buffer text;
    sxml_xml_buffer value;
    /* The attribute names of the element just started. */
    sxml_xml_array attribute_names; /* sxml_name_identifier */
    size_t element_depth;
    size_t document_element_count;
    sxml_ixml_construction_error error;
    /* The element whose attributes are being sent. */
    sxml_byte_view element_name;
    /* What the first error is about (sxml_ixml_construction_outcome). */
    sxml_byte_view error_name;
    sxml_byte_view error_element_name;
} xml_emitter;

/* The arrays are read directly: a node is read several times per
   character of the input. */
static const sxml_ixml_tree_node *tree_node(const xml_emitter *emitter, uint32_t node_index) {
    return sxml_ixml_parse_result_node(emitter->request->parse, node_index);
}

static const sxml_ixml_symbol *grammar_symbol(const sxml_ixml_grammar *grammar, uint32_t symbol) {
    return &((const sxml_ixml_symbol *)grammar->symbols.elements)[symbol];
}

static const sxml_ixml_nonterminal *grammar_nonterminal(const sxml_ixml_grammar *grammar,
                                                        uint32_t nonterminal) {
    return &((const sxml_ixml_nonterminal *)grammar->nonterminals.elements)[nonterminal];
}

/* The mark of a node: the use's, else the rule's, else included (the
   Specification, "Constructing XML"). A terminal is ELEMENT or HIDDEN. */
static sxml_ixml_mark node_mark(const xml_emitter *emitter, const sxml_ixml_tree_node *node) {
    sxml_ixml_mark mark = SXML_IXML_MARK_NONE;
    if (node->symbol != SXML_IXML_NONE) {
        mark = (sxml_ixml_mark)grammar_symbol(emitter->grammar, node->symbol)->mark;
    }
    if (mark == SXML_IXML_MARK_NONE && node->node_kind == SXML_IXML_NODE_KIND_NONTERMINAL) {
        mark = (sxml_ixml_mark)grammar_nonterminal(emitter->grammar, node->detail)->mark;
    }
    return mark == SXML_IXML_MARK_NONE ? SXML_IXML_MARK_ELEMENT : mark;
}

/* The name of a nonterminal node: the use's alias, else the rule's, else
   its name. */
static sxml_byte_view node_name(const xml_emitter *emitter, const sxml_ixml_tree_node *node) {
    if (node->symbol != SXML_IXML_NONE) {
        const sxml_ixml_symbol *symbol = grammar_symbol(emitter->grammar, node->symbol);
        if (symbol->text.byte_count > 0) {
            return sxml_ixml_grammar_text(emitter->grammar, symbol->text);
        }
    }
    const sxml_ixml_nonterminal *nonterminal = grammar_nonterminal(emitter->grammar, node->detail);
    if (nonterminal->alias.byte_count > 0) {
        return sxml_ixml_grammar_text(emitter->grammar, nonterminal->alias);
    }
    return sxml_ixml_grammar_text(emitter->grammar, nonterminal->name);
}

static void fail_constructing(xml_emitter *emitter, sxml_ixml_construction_error error) {
    if (emitter->error == SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        emitter->error = error;
    }
}

/* Fails with error, which is about the name name (D02, D03, D07). */
typedef struct named_failure {
    sxml_ixml_construction_error error;
    sxml_byte_view name;
} named_failure;

static void fail_constructing_name(xml_emitter *emitter, named_failure failure) {
    if (emitter->error == SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        emitter->error_name = failure.name;
        emitter->error_element_name = emitter->element_name;
    }
    fail_constructing(emitter, failure.error);
}

/* Appends codepoint to text, or fails with D04 for a character XML does
   not allow. */
static sxml_status append_character(xml_emitter *emitter, sxml_xml_buffer *text,
                                    uint32_t codepoint) {
    if (!sxml_xml_codepoint_is_char(codepoint)) {
        fail_constructing(emitter, SXML_IXML_CONSTRUCTION_ERROR_CHARACTER);
        return SXML_STATUS_OK;
    }
    return sxml_xml_buffer_append_codepoint(text, codepoint);
}

/* Appends the text a terminal or insertion node adds, if any. */
static sxml_status append_leaf(xml_emitter *emitter, const sxml_ixml_tree_node *node,
                               sxml_xml_buffer *text) {
    if (node->node_kind == SXML_IXML_NODE_KIND_TERMINAL) {
        if (node_mark(emitter, node) == SXML_IXML_MARK_HIDDEN) {
            return SXML_STATUS_OK;
        }
        return append_character(emitter, text, emitter->request->input.codepoints[node->detail]);
    }
    if (node->node_kind == SXML_IXML_NODE_KIND_INSERTION) {
        const sxml_byte_view inserted = sxml_ixml_grammar_text(
            emitter->grammar, grammar_symbol(emitter->grammar, node->symbol)->text);
        sxml_status status = SXML_STATUS_OK;
        for (size_t byte_offset = 0; status == SXML_STATUS_OK && byte_offset < inserted.byte_count;) {
            const sxml_xml_decoded_codepoint decoded =
                sxml_xml_decode_codepoint(inserted, byte_offset);
            byte_offset += decoded.encoded_byte_count;
            status = append_character(emitter, text, decoded.codepoint);
        }
        return status;
    }
    return SXML_STATUS_OK;
}

/* The node after node in a walk of the subtree of top, in document order,
   going into node's children only when descends; SXML_IXML_NONE at the
   end. */
typedef struct walk_step {
    uint32_t node;
    uint32_t top;
    bool descends;
} walk_step;

static uint32_t next_in_walk(const xml_emitter *emitter, walk_step step) {
    const sxml_ixml_tree_node *node = tree_node(emitter, step.node);
    if (step.descends && node->first_child != SXML_IXML_NONE) {
        return node->first_child;
    }
    uint32_t current = step.node;
    while (current != step.top) {
        node = tree_node(emitter, current);
        if (node->next_sibling != SXML_IXML_NONE) {
            return node->next_sibling;
        }
        current = node->parent;
    }
    return SXML_IXML_NONE;
}

/* Interns a no-namespace name, checking that it is an XML name (D03). */
static sxml_status intern_name(xml_emitter *emitter, sxml_byte_view local_name,
                               sxml_name_identifier *name) {
    if (!sxml_xml_text_is_ncname(local_name)) {
        fail_constructing_name(
            emitter, (named_failure){.error = SXML_IXML_CONSTRUCTION_ERROR_NAME, .name = local_name});
        return SXML_STATUS_OK;
    }
    return sxml_name_pool_intern_name(
        emitter->request->name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = local_name},
        name);
}

/* Sends an attribute of the element just started, once per name (D02). */
typedef struct attribute_output {
    sxml_name_identifier attribute_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_byte_view value;
    /* The attribute's local name, for D02. */
    sxml_byte_view local_name;
} attribute_output;

static sxml_status send_attribute(xml_emitter *emitter, const attribute_output *output) {
    for (size_t name_index = 0; name_index < emitter->attribute_names.element_count;
         name_index += 1) {
        const sxml_name_identifier *known = sxml_xml_array_at(&emitter->attribute_names, name_index);
        if (known->index == output->attribute_name.index) {
            fail_constructing_name(emitter,
                                   (named_failure){.error = SXML_IXML_CONSTRUCTION_ERROR_DUPLICATE_ATTRIBUTE,
                                                   .name = output->local_name});
            return SXML_STATUS_OK;
        }
    }
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(&emitter->attribute_names, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *(sxml_name_identifier *)element = output->attribute_name;
    return emitter->builder.operations->add_attribute(
        emitter->builder.state,
        &(sxml_attribute_event){.attribute_name = output->attribute_name,
                                .namespace_prefix = output->namespace_prefix,
                                .attribute_value = output->value,
                                .type_annotation = {.index = SXML_TYPE_NONE},
                                .is_id = false,
                                .is_idrefs = false,
                                .location = {.line_number = 0}});
}

/* Sends the attribute an attribute node makes: its name, and the text of
   every terminal descendant not deleted and every insertion. */
static sxml_status emit_attribute(xml_emitter *emitter, uint32_t attribute_node) {
    const sxml_byte_view local_name = node_name(emitter, tree_node(emitter, attribute_node));
    if (sxml_byte_view_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("xmlns"))) {
        fail_constructing_name(emitter,
                               (named_failure){.error = SXML_IXML_CONSTRUCTION_ERROR_XMLNS_ATTRIBUTE,
                                               .name = local_name});
        return SXML_STATUS_OK;
    }
    sxml_name_identifier attribute_name = {.index = 0};
    sxml_status status = intern_name(emitter, local_name, &attribute_name);
    emitter->value.byte_count = 0;
    for (uint32_t descendant = next_in_walk(emitter, (walk_step){.node = attribute_node,
                                                                 .top = attribute_node,
                                                                 .descends = true});
         status == SXML_STATUS_OK && descendant != SXML_IXML_NONE;
         descendant = next_in_walk(emitter, (walk_step){.node = descendant,
                                                        .top = attribute_node,
                                                        .descends = true})) {
        status = append_leaf(emitter, tree_node(emitter, descendant), &emitter->value);
    }
    if (status != SXML_STATUS_OK || emitter->error != SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        return status;
    }
    return send_attribute(emitter,
                          &(attribute_output){.attribute_name = attribute_name,
                                              .namespace_prefix = {.index = 0},
                                              .value = {.bytes = emitter->value.bytes,
                                                        .byte_count = emitter->value.byte_count},
                                              .local_name = local_name});
}

/* Sends the exposed attributes of element_node: its attribute children
   and those of its deleted descendants, through deleted nonterminals
   only, and only into those that can expose one (the grammar's
   exposes_attributes, D283). */
static sxml_status emit_exposed_attributes(xml_emitter *emitter, uint32_t element_node) {
    const bool *exposes = emitter->grammar->tables.exposes_attributes;
    if (!exposes[tree_node(emitter, element_node)->detail]) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    uint32_t node = next_in_walk(
        emitter, (walk_step){.node = element_node, .top = element_node, .descends = true});
    while (status == SXML_STATUS_OK && emitter->error == SXML_IXML_CONSTRUCTION_ERROR_NONE &&
           node != SXML_IXML_NONE) {
        const sxml_ixml_tree_node *current = tree_node(emitter, node);
        bool descends = false;
        if (current->node_kind == SXML_IXML_NODE_KIND_NONTERMINAL) {
            const sxml_ixml_mark mark = node_mark(emitter, current);
            if (mark == SXML_IXML_MARK_ATTRIBUTE) {
                status = emit_attribute(emitter, node);
            }
            descends = mark == SXML_IXML_MARK_HIDDEN && exposes[current->detail];
        }
        node = next_in_walk(emitter,
                            (walk_step){.node = node, .top = element_node, .descends = descends});
    }
    return status;
}

/* A name in the ixml namespace and the prefix to write it with. */
typedef struct ixml_qualified_name {
    sxml_name_identifier name;
    sxml_namespace_prefix_identifier namespace_prefix;
} ixml_qualified_name;

static sxml_status intern_ixml_name(xml_emitter *emitter, sxml_byte_view local_name,
                                    ixml_qualified_name *qualified) {
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        emitter->request->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(SXML_IXML_NAMESPACE_URI),
        &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(emitter->request->name_pool,
                                                        SXML_BYTE_VIEW_FROM_LITERAL("ixml"),
                                                        &qualified->namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            emitter->request->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
            &qualified->name);
    }
    return status;
}

/* An ixml: attribute of the document element. */
typedef struct ixml_attribute {
    sxml_byte_view local_name;
    sxml_byte_view value;
} ixml_attribute;

static sxml_status send_ixml_attribute(xml_emitter *emitter, ixml_attribute attribute) {
    ixml_qualified_name qualified = {.name = {.index = 0}, .namespace_prefix = {.index = 0}};
    const sxml_status status = intern_ixml_name(emitter, attribute.local_name, &qualified);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return send_attribute(emitter, &(attribute_output){.attribute_name = qualified.name,
                                                       .namespace_prefix = qualified.namespace_prefix,
                                                       .value = attribute.value,
                                                       .local_name = attribute.local_name});
}

/* The ixml:state and ixml:version of the document element (the
   Specification, "Parsing" and "Prolog"). */
static sxml_status emit_state(xml_emitter *emitter, bool has_failed) {
    const bool is_ambiguous = !has_failed && emitter->request->parse->is_ambiguous;
    const bool is_mismatch = emitter->grammar->has_version_mismatch;
    if (!has_failed && !is_ambiguous && !is_mismatch) {
        return SXML_STATUS_OK;
    }
    sxml_xml_buffer *state = &emitter->value;
    state->byte_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (has_failed) {
        status = sxml_xml_buffer_append(state, SXML_BYTE_VIEW_FROM_LITERAL("failed"));
    } else if (is_ambiguous) {
        status = sxml_xml_buffer_append(state, SXML_BYTE_VIEW_FROM_LITERAL("ambiguous"));
    }
    if (status == SXML_STATUS_OK && is_mismatch) {
        status = sxml_xml_buffer_append(state, state->byte_count > 0
                                                   ? SXML_BYTE_VIEW_FROM_LITERAL(" version-mismatch")
                                                   : SXML_BYTE_VIEW_FROM_LITERAL("version-mismatch"));
    }
    if (status == SXML_STATUS_OK) {
        status = send_ixml_attribute(
            emitter,
            (ixml_attribute){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("state"),
                             .value = {.bytes = state->bytes, .byte_count = state->byte_count}});
    }
    if (status == SXML_STATUS_OK && is_mismatch) {
        status = send_ixml_attribute(
            emitter, (ixml_attribute){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("version"),
                                      .value = SXML_BYTE_VIEW_FROM_LITERAL("1.1")});
    }
    return status;
}

/* Sends the text gathered since the last element event. */
static sxml_status flush_text(xml_emitter *emitter) {
    if (emitter->text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    if (emitter->element_depth == 0) {
        /* Text beside the document element (D06). */
        fail_constructing(emitter, SXML_IXML_CONSTRUCTION_ERROR_ROOT_ELEMENT);
        return SXML_STATUS_OK;
    }
    const sxml_status status = emitter->builder.operations->add_text(
        emitter->builder.state,
        &(sxml_text_event){.text = {.bytes = emitter->text.bytes,
                                    .byte_count = emitter->text.byte_count},
                           .location = {.line_number = 0},
                           .is_cdata_section = false,
                           .disables_output_escaping = false});
    emitter->text.byte_count = 0;
    return status;
}

/* Starts an element named local_name; the document element gets the
   state attributes after the element's own. */
typedef struct element_output {
    sxml_byte_view local_name;
    /* The node whose exposed attributes the element has, or
       SXML_IXML_NONE. */
    uint32_t attribute_owner;
    bool has_failed;
} element_output;

static sxml_status start_element(xml_emitter *emitter, const element_output *output) {
    sxml_status status = flush_text(emitter);
    if (emitter->element_depth == 0) {
        emitter->document_element_count += 1;
        if (emitter->document_element_count > 1) {
            fail_constructing(emitter, SXML_IXML_CONSTRUCTION_ERROR_ROOT_ELEMENT);
        }
    }
    sxml_name_identifier element_name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = intern_name(emitter, output->local_name, &element_name);
    }
    if (status != SXML_STATUS_OK || emitter->error != SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        return status;
    }
    status = emitter->builder.operations->start_element(
        emitter->builder.state,
        &(sxml_element_start_event){.element_name = element_name,
                                    .namespace_prefix = {.index = 0},
                                    .type_annotation = {.index = SXML_TYPE_NONE},
                                    .is_id = false,
                                    .is_idrefs = false,
                                    .is_nilled = false,
                                    .ignores_parent_namespaces = false,
                                    .location = {.line_number = 0},
                                    .entity_base_uri = {.bytes = NULL, .byte_count = 0}});
    emitter->attribute_names.element_count = 0;
    emitter->element_name = output->local_name;
    if (status == SXML_STATUS_OK && output->attribute_owner != SXML_IXML_NONE) {
        status = emit_exposed_attributes(emitter, output->attribute_owner);
    }
    if (status == SXML_STATUS_OK && emitter->element_depth == 0) {
        status = emit_state(emitter, output->has_failed);
    }
    emitter->element_depth += 1;
    return status;
}

static sxml_status end_element(xml_emitter *emitter) {
    const sxml_status status = flush_text(emitter);
    emitter->element_depth -= 1;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return emitter->builder.operations->end_element(emitter->builder.state);
}

/* Handles node as a walk enters it; *descends says whether to go into
   its children. */
static sxml_status enter_node(xml_emitter *emitter, uint32_t node_index, bool *descends) {
    const sxml_ixml_tree_node *node = tree_node(emitter, node_index);
    *descends = false;
    if (node->node_kind != SXML_IXML_NODE_KIND_NONTERMINAL) {
        return append_leaf(emitter, node, &emitter->text);
    }
    switch (node_mark(emitter, node)) {
    case SXML_IXML_MARK_ELEMENT:
        *descends = true;
        return start_element(emitter, &(element_output){.local_name = node_name(emitter, node),
                                                        .attribute_owner = node_index,
                                                        .has_failed = false});
    case SXML_IXML_MARK_HIDDEN:
        *descends = true;
        if (node_index == 0) {
            /* Attributes a deleted root exposes have no element (D05). */
            for (uint32_t current = next_in_walk(
                     emitter, (walk_step){.node = 0, .top = 0, .descends = true});
                 current != SXML_IXML_NONE;) {
                const sxml_ixml_tree_node *candidate = tree_node(emitter, current);
                bool goes_in = false;
                if (candidate->node_kind == SXML_IXML_NODE_KIND_NONTERMINAL) {
                    const sxml_ixml_mark mark = node_mark(emitter, candidate);
                    if (mark == SXML_IXML_MARK_ATTRIBUTE) {
                        fail_constructing(emitter, SXML_IXML_CONSTRUCTION_ERROR_ROOT_ATTRIBUTE);
                    }
                    goes_in = mark == SXML_IXML_MARK_HIDDEN;
                }
                current = next_in_walk(
                    emitter, (walk_step){.node = current, .top = 0, .descends = goes_in});
            }
        }
        return SXML_STATUS_OK;
    case SXML_IXML_MARK_ATTRIBUTE:
        if (node_index == 0) {
            fail_constructing(emitter, SXML_IXML_CONSTRUCTION_ERROR_ROOT_ATTRIBUTE);
        }
        /* Sent by the element that exposes it. */
        return SXML_STATUS_OK;
    case SXML_IXML_MARK_NONE:
        break;
    }
    return SXML_STATUS_OK;
}

/* Handles node as a walk leaves it. */
static sxml_status leave_node(xml_emitter *emitter, uint32_t node_index) {
    const sxml_ixml_tree_node *node = tree_node(emitter, node_index);
    if (node->node_kind == SXML_IXML_NODE_KIND_NONTERMINAL &&
        node_mark(emitter, node) == SXML_IXML_MARK_ELEMENT) {
        return end_element(emitter);
    }
    return SXML_STATUS_OK;
}

/* Sends the document of a sentence's parse tree. */
static sxml_status emit_sentence(xml_emitter *emitter) {
    uint32_t node = 0;
    bool is_entering = true;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && emitter->error == SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        if (is_entering) {
            bool descends = false;
            status = enter_node(emitter, node, &descends);
            const uint32_t first_child = tree_node(emitter, node)->first_child;
            if (descends && first_child != SXML_IXML_NONE) {
                node = first_child;
            } else {
                is_entering = false;
            }
            continue;
        }
        status = leave_node(emitter, node);
        if (node == 0) {
            break;
        }
        const sxml_ixml_tree_node *current = tree_node(emitter, node);
        if (current->next_sibling != SXML_IXML_NONE) {
            node = current->next_sibling;
            is_entering = true;
        } else {
            node = current->parent;
        }
    }
    if (status == SXML_STATUS_OK && emitter->error == SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        status = flush_text(emitter);
        if (emitter->document_element_count != 1) {
            /* D06: exactly one element at the top. */
            fail_constructing(emitter, SXML_IXML_CONSTRUCTION_ERROR_ROOT_ELEMENT);
        }
    }
    return status;
}

/* --------------------------------------------------- Failure document */

/* Writes a character as the grammar would: in double quotes, a double
   quote in single quotes, and "#" and hex digits when it is not
   printable. */
sxml_status sxml_ixml_append_character_notation(sxml_xml_buffer *text, uint32_t codepoint) {
    static const char hex_digits[] = "0123456789abcdef";
    if (codepoint > 0x20U && codepoint != 0x7FU && !(codepoint >= 0x80U && codepoint <= 0x9FU) &&
        sxml_xml_codepoint_is_char(codepoint)) {
        const uint8_t quote = codepoint == '"' ? (uint8_t)'\'' : (uint8_t)'"';
        sxml_status status = sxml_xml_buffer_append_byte(text, quote);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_codepoint(text, codepoint);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(text, quote);
        }
        return status;
    }
    uint8_t digits[8];
    size_t digit_count = 0;
    uint32_t rest = codepoint;
    do {
        digits[digit_count] = (uint8_t)hex_digits[rest & 0xFU];
        digit_count += 1;
        rest >>= 4U;
    } while (rest != 0 && digit_count < sizeof(digits));
    sxml_status status = sxml_xml_buffer_append_byte(text, '#');
    while (status == SXML_STATUS_OK && digit_count > 0) {
        digit_count -= 1;
        status = sxml_xml_buffer_append_byte(text, digits[digit_count]);
    }
    return status;
}

/* Appends a terminal in the grammar's notation: "a", ["a"-"z"; Lu],
   ~["{}"]. */
static sxml_status append_terminal_notation(const sxml_ixml_grammar *grammar, uint32_t terminal_index,
                                            sxml_xml_buffer *text) {
    static const char category_codes[] = "LuLlLtLmLoMnMcMeNdNlNoPcPdPsPePiPfPoZsZlZpSmScSkSoCcCfCsCoCn";
    const sxml_ixml_terminal *terminal = sxml_xml_array_at(&grammar->terminals, terminal_index);
    const sxml_ixml_codepoint_range *first =
        terminal->range_count == 0 ? NULL : sxml_xml_array_at(&grammar->ranges, terminal->first_range);
    if (!terminal->is_exclusion && terminal->range_count == 1 && terminal->category_mask == 0 &&
        first->first_codepoint == first->last_codepoint) {
        return sxml_ixml_append_character_notation(text, first->first_codepoint);
    }
    sxml_status status = sxml_xml_buffer_append(
        text, terminal->is_exclusion ? SXML_BYTE_VIEW_FROM_LITERAL("~[")
                                     : SXML_BYTE_VIEW_FROM_LITERAL("["));
    bool is_first_member = true;
    for (uint32_t range_index = 0; status == SXML_STATUS_OK && range_index < terminal->range_count;
         range_index += 1) {
        const sxml_ixml_codepoint_range *range =
            sxml_xml_array_at(&grammar->ranges, terminal->first_range + range_index);
        if (!is_first_member) {
            status = sxml_xml_buffer_append(text, SXML_BYTE_VIEW_FROM_LITERAL("; "));
        }
        is_first_member = false;
        if (status == SXML_STATUS_OK) {
            status = sxml_ixml_append_character_notation(text, range->first_codepoint);
        }
        if (status == SXML_STATUS_OK && range->last_codepoint != range->first_codepoint) {
            status = sxml_xml_buffer_append_byte(text, '-');
            if (status == SXML_STATUS_OK) {
                status = sxml_ixml_append_character_notation(text, range->last_codepoint);
            }
        }
    }
    /* A whole major class is written as its letter. */
    static const uint32_t class_masks[] = {
        SXML_REGEX_CATEGORY_MASK_L, SXML_REGEX_CATEGORY_MASK_M, SXML_REGEX_CATEGORY_MASK_N,
        SXML_REGEX_CATEGORY_MASK_P, SXML_REGEX_CATEGORY_MASK_Z, SXML_REGEX_CATEGORY_MASK_S,
        SXML_REGEX_CATEGORY_MASK_C};
    static const char class_letters[] = "LMNPZSC";
    uint32_t remaining_mask = terminal->category_mask;
    for (size_t class_index = 0; status == SXML_STATUS_OK && class_index < 7; class_index += 1) {
        if ((remaining_mask & class_masks[class_index]) != class_masks[class_index]) {
            continue;
        }
        remaining_mask &= ~class_masks[class_index];
        if (!is_first_member) {
            status = sxml_xml_buffer_append(text, SXML_BYTE_VIEW_FROM_LITERAL("; "));
        }
        is_first_member = false;
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(text, (uint8_t)class_letters[class_index]);
        }
    }
    for (uint32_t category = 0; status == SXML_STATUS_OK && category <= SXML_REGEX_GENERAL_CATEGORY_CN;
         category += 1) {
        if ((remaining_mask & (1U << category)) == 0) {
            continue;
        }
        if (!is_first_member) {
            status = sxml_xml_buffer_append(text, SXML_BYTE_VIEW_FROM_LITERAL("; "));
        }
        is_first_member = false;
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(
                text, (sxml_byte_view){.bytes = (const uint8_t *)&category_codes[(size_t)category * 2],
                                       .byte_count = 2});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append_byte(text, ']');
    }
    return status;
}

/* An element of the failure document with text content. */
typedef struct failure_field {
    sxml_byte_view local_name;
    sxml_byte_view content;
} failure_field;

static sxml_status emit_failure_field(xml_emitter *emitter, failure_field field) {
    sxml_status status = start_element(emitter, &(element_output){.local_name = field.local_name,
                                                                  .attribute_owner = SXML_IXML_NONE,
                                                                  .has_failed = true});
    emitter->text.byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&emitter->text, field.content);
    }
    if (status == SXML_STATUS_OK) {
        status = end_element(emitter);
    }
    return status;
}

/* Writes a decimal number to text. */
static sxml_status append_decimal(sxml_xml_buffer *text, size_t number) {
    uint8_t digits[24];
    size_t digit_count = 0;
    size_t rest = number;
    do {
        digits[digit_count] = (uint8_t)('0' + rest % 10);
        digit_count += 1;
        rest /= 10;
    } while (rest != 0);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && digit_count > 0) {
        digit_count -= 1;
        status = sxml_xml_buffer_append_byte(text, digits[digit_count]);
    }
    return status;
}

sxml_ixml_failure_location sxml_ixml_locate_failure(sxml_ixml_failure_request request) {
    sxml_ixml_failure_location location = {
        .line_number = 1, .column_number = 1, .codepoint = SXML_IXML_NONE};
    const size_t failure_position = request.parse->failure_position;
    for (size_t position = 0; position < failure_position; position += 1) {
        if (request.input.codepoints[position] == '\n') {
            location.line_number += 1;
            location.column_number = 1;
        } else {
            location.column_number += 1;
        }
    }
    if (failure_position < request.input.codepoint_count) {
        location.codepoint = request.input.codepoints[failure_position];
    }
    return location;
}

/* The notations of the expected terminals, in one buffer. */
typedef struct expected_notations {
    sxml_xml_buffer text;
    sxml_xml_array ranges; /* sxml_ixml_text_range */
} expected_notations;

static int compare_notations(const expected_notations *notations, sxml_ixml_text_range left,
                             sxml_ixml_text_range right) {
    const size_t common = left.byte_count < right.byte_count ? left.byte_count : right.byte_count;
    const int order = common == 0 ? 0
                                  : memcmp(notations->text.bytes + left.byte_offset,
                                           notations->text.bytes + right.byte_offset, common);
    if (order != 0 || left.byte_count == right.byte_count) {
        return order;
    }
    return left.byte_count < right.byte_count ? -1 : 1;
}

/* Writes the notation of each expected terminal and sorts them by their
   UTF-8 bytes, which is code-point order (an insertion sort: a failure
   expects few terminals). */
static sxml_status collect_expected(xml_emitter *emitter, expected_notations *notations) {
    const sxml_ixml_parse_result *parse = emitter->request->parse;
    sxml_status status = SXML_STATUS_OK;
    for (size_t expected_index = 0;
         status == SXML_STATUS_OK && expected_index < parse->expected_terminals.element_count;
         expected_index += 1) {
        const size_t byte_offset = notations->text.byte_count;
        status = append_terminal_notation(
            emitter->grammar,
            *(const uint32_t *)sxml_xml_array_at(&parse->expected_terminals, expected_index),
            &notations->text);
        void *element = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_array_push(&notations->ranges, &element);
        }
        if (status == SXML_STATUS_OK) {
            *(sxml_ixml_text_range *)element = (sxml_ixml_text_range){
                .byte_offset = (uint32_t)byte_offset,
                .byte_count = (uint32_t)(notations->text.byte_count - byte_offset)};
        }
    }
    sxml_ixml_text_range *ranges = (sxml_ixml_text_range *)notations->ranges.elements;
    for (size_t sorted_count = 1;
         status == SXML_STATUS_OK && sorted_count < notations->ranges.element_count;
         sorted_count += 1) {
        const sxml_ixml_text_range moving = ranges[sorted_count];
        size_t place = sorted_count;
        while (place > 0 && compare_notations(notations, ranges[place - 1], moving) > 0) {
            ranges[place] = ranges[place - 1];
            place -= 1;
        }
        ranges[place] = moving;
    }
    return status;
}

/* Sends the document of a failed parse (the Specification, "Parsing":
   where and why it failed): <failed ixml:state="failed"> with the line,
   column and character position of the failure, the character found
   there (or end-of-input), and each terminal that was expected, once,
   in code-point order of its notation. */
static sxml_status emit_failure(xml_emitter *emitter) {
    const sxml_ixml_parse_result *parse = emitter->request->parse;
    const sxml_ixml_failure_location location = sxml_ixml_locate_failure(
        (sxml_ixml_failure_request){.parse = parse, .input = emitter->request->input});
    sxml_status status = start_element(emitter, &(element_output){
                                                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("failed"),
                                                    .attribute_owner = SXML_IXML_NONE,
                                                    .has_failed = true});
    sxml_xml_buffer number = sxml_xml_buffer_empty(emitter->request->allocator);
    const size_t numbers[3] = {location.line_number, location.column_number,
                               parse->failure_position + 1};
    static const sxml_byte_view number_names[3] = {SXML_BYTE_VIEW_INITIALIZER("line"),
                                                   SXML_BYTE_VIEW_INITIALIZER("column"),
                                                   SXML_BYTE_VIEW_INITIALIZER("position")};
    for (size_t field_index = 0; status == SXML_STATUS_OK && field_index < 3; field_index += 1) {
        number.byte_count = 0;
        status = append_decimal(&number, numbers[field_index]);
        if (status == SXML_STATUS_OK) {
            status = emit_failure_field(
                emitter, (failure_field){.local_name = number_names[field_index],
                                         .content = {.bytes = number.bytes,
                                                     .byte_count = number.byte_count}});
        }
    }
    number.byte_count = 0;
    if (status == SXML_STATUS_OK && location.codepoint != SXML_IXML_NONE) {
        status = sxml_ixml_append_character_notation(&number, location.codepoint);
        if (status == SXML_STATUS_OK) {
            status = emit_failure_field(
                emitter,
                (failure_field){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("unexpected"),
                                .content = {.bytes = number.bytes, .byte_count = number.byte_count}});
        }
    } else if (status == SXML_STATUS_OK) {
        status = emit_failure_field(
            emitter, (failure_field){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("end-of-input"),
                                     .content = {.bytes = NULL, .byte_count = 0}});
    }
    sxml_xml_buffer_release(&number);
    expected_notations notations = {
        .text = sxml_xml_buffer_empty(emitter->request->allocator),
        .ranges = empty_array(emitter->request->allocator, sizeof(sxml_ixml_text_range))};
    if (status == SXML_STATUS_OK) {
        status = collect_expected(emitter, &notations);
    }
    const sxml_ixml_text_range *ranges = (const sxml_ixml_text_range *)notations.ranges.elements;
    for (size_t range_index = 0;
         status == SXML_STATUS_OK && range_index < notations.ranges.element_count;
         range_index += 1) {
        if (range_index > 0 &&
            compare_notations(&notations, ranges[range_index - 1], ranges[range_index]) == 0) {
            continue;
        }
        status = emit_failure_field(
            emitter, (failure_field){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("expected"),
                                     .content = {.bytes = notations.text.bytes +
                                                          ranges[range_index].byte_offset,
                                                 .byte_count = ranges[range_index].byte_count}});
    }
    sxml_xml_buffer_release(&notations.text);
    sxml_xml_array_release(&notations.ranges);
    if (status == SXML_STATUS_OK) {
        status = end_element(emitter);
    }
    return status;
}

sxml_status sxml_ixml_construct(const sxml_ixml_construction_request *request,
                                sxml_ixml_construction_outcome *outcome) {
    const sxml_allocator allocator = request->allocator;
    xml_emitter emitter = {
        .request = request,
        .grammar = request->grammar,
        .builder = request->builder,
        .text = sxml_xml_buffer_empty(allocator),
        .value = sxml_xml_buffer_empty(allocator),
        .attribute_names = empty_array(allocator, sizeof(sxml_name_identifier)),
        .element_depth = 0,
        .document_element_count = 0,
        .error = SXML_IXML_CONSTRUCTION_ERROR_NONE,
        .element_name = {.bytes = NULL, .byte_count = 0},
        .error_name = {.bytes = NULL, .byte_count = 0},
        .error_element_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = request->builder.operations->start_document(
        request->builder.state, &(sxml_document_start_event){.document_uri = {.bytes = NULL,
                                                                              .byte_count = 0},
                                                             .base_uri = {.bytes = NULL,
                                                                          .byte_count = 0},
                                                             .location = {.line_number = 0}});
    if (status == SXML_STATUS_OK) {
        status = request->parse->is_sentence ? emit_sentence(&emitter) : emit_failure(&emitter);
    }
    if (status == SXML_STATUS_OK && emitter.error == SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        status = request->builder.operations->end_document(request->builder.state);
    }
    outcome->error = emitter.error;
    outcome->name = emitter.error_name;
    outcome->element_name = emitter.error_element_name;
    sxml_xml_buffer_release(&emitter.text);
    sxml_xml_buffer_release(&emitter.value);
    sxml_xml_array_release(&emitter.attribute_names);
    return status;
}

/* --------------------------------------------------------- The feature */

const sxml_optional_feature_definition sxml_invisible_xml_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_INVISIBLE_XML,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("invisible-xml"),
    .operations = &sxml_ixml_library_functions,
    .finalize_state = NULL,
    .library_functions = &sxml_ixml_library_functions,
};

sxml_optional_feature sxml_invisible_xml_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_invisible_xml_feature_definition};
}
