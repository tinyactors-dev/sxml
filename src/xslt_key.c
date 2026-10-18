/*
 * Keys and the XSLT function table (src/xslt_key_internal.h; XSLT 4.0,
 * sections 20.2 and 11.9.2; docs/DECISIONS.md D101, D118, D149; packet
 * 4.10).
 *
 * The stylesheet functions are IR functions lowered from XPath texts made
 * here (as the built-in templates of src/xslt_lower_template.c are, D144),
 * in a static context of their own whose function table adds private names:
 * the internal rows key-name, key-index and key-normalize (no namespace,
 * src/function_library_xslt.c) and the IR functions match-N, use-N and
 * build-K in the namespace KEY_FUNCTION_NAMESPACE_URI. For a key k with
 * declarations d1, d2, the build function of k is
 *
 *     map:build(($root/descendant-or-self::node()
 *                | $root/descendant-or-self::*\/@*),
 *               fn($node) {
 *                 (if (match-1($node))
 *                  then key-normalize(use-1($node), "collation", false())
 *                  else ()),
 *                 (if (match-2($node)) then ... else ())
 *               })
 *
 * where match-N is the match function of dN's pattern (D118) inside a
 * TRY_CATCH that makes every error false (6.5), and use-N the use
 * attribute or sequence constructor of dN with the node as its fixed
 * focus. fn:key#3 is
 *
 *     let $name := key-name($key-name), $root := root($top)
 *     return (if ($name eq QName("u", "k"))
 *             then let $index := key-index(1, $root, build-1#1)
 *                  return for $value in key-normalize($key-value, "c", false())
 *                         return $index($value)
 *             else if ... else error(XTDE1260))
 *            [exists(ancestor-or-self::node()[. is $top])]/.
 *
 * and fn:key#2 the same with $root := root($context) checked to be a
 * document node (XTDE1270) and without the filter. "/." puts the nodes in
 * document order without duplicates.
 *
 * fn:copy-of is the copy of xsl:copy-of (sxml_xslt_lowering_bind_copy_of);
 * fn:snapshot an XPath text over fn:copy-of and computed constructors
 * (snapshot_text).
 *
 * Golden file: src/xslt_lower_template.c (functions lowered outside a
 * machine one after another, snippets parsed once per lowering).
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_key_internal.h"

#include "function_library_core_internal.h"
#include "function_library_parts_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"
#include "xslt_accumulator_internal.h"
#include "xslt_pattern_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The namespace of the private functions of the key texts. */
#define KEY_FUNCTION_NAMESPACE_URI "urn:x-sxml:key"

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* ---------------------------------------------------- Stylesheet functions */

/* The stylesheet functions an expression may name. */
typedef enum stylesheet_function_kind {
    /* fn:key#2: (key-name, key-value, the caller's context value). */
    SXML_STYLESHEET_FUNCTION_KEY_WITH_CONTEXT,
    /* fn:key#3: (key-name, key-value, top). */
    SXML_STYLESHEET_FUNCTION_KEY_WITH_TOP,
    /* fn:copy-of#1 (and #0, which passes the context value). */
    SXML_STYLESHEET_FUNCTION_COPY_OF,
    /* fn:snapshot#1 (and #0). */
    SXML_STYLESHEET_FUNCTION_SNAPSHOT,
    SXML_STYLESHEET_FUNCTION_COUNT,
} stylesheet_function_kind;

static const sxml_function_parameter key_with_context_parameters[] = {
    SXML_FUNCTION_PARAMETER("key-name", "xs:anyAtomicType", SXML_PARAMETER_DEFAULT_REQUIRED),
    SXML_FUNCTION_PARAMETER("key-value", "xs:anyAtomicType*", SXML_PARAMETER_DEFAULT_REQUIRED),
    SXML_FUNCTION_PARAMETER("context", "node()", SXML_PARAMETER_DEFAULT_CONTEXT_VALUE),
};

static const sxml_function_parameter key_with_top_parameters[] = {
    SXML_FUNCTION_PARAMETER("key-name", "xs:anyAtomicType", SXML_PARAMETER_DEFAULT_REQUIRED),
    SXML_FUNCTION_PARAMETER("key-value", "xs:anyAtomicType*", SXML_PARAMETER_DEFAULT_REQUIRED),
    SXML_FUNCTION_PARAMETER("top", "node()", SXML_PARAMETER_DEFAULT_REQUIRED),
};

static const sxml_function_parameter copy_of_parameters[] = {
    SXML_FUNCTION_PARAMETER("input", "item()*", SXML_PARAMETER_DEFAULT_CONTEXT_VALUE),
};

/* What each kind is: its fn name, its parameters (all of them are the IR
   function's), the arity an expression names it with that passes the
   last parameter itself (the context value), and its result. */
typedef struct stylesheet_function_rule {
    sxml_byte_view local_name;
    sxml_function_parameter_view parameters;
    size_t named_arity;
    sxml_byte_view result_type;
} stylesheet_function_rule;

static const stylesheet_function_rule stylesheet_function_rules[SXML_STYLESHEET_FUNCTION_COUNT] = {
    [SXML_STYLESHEET_FUNCTION_KEY_WITH_CONTEXT] = {.local_name = SXML_BYTE_VIEW_INITIALIZER("key"),
                                                   .parameters = {key_with_context_parameters, 3},
                                                   .named_arity = 2,
                                                   .result_type =
                                                       SXML_BYTE_VIEW_INITIALIZER("node()*")},
    [SXML_STYLESHEET_FUNCTION_KEY_WITH_TOP] = {.local_name = SXML_BYTE_VIEW_INITIALIZER("key"),
                                               .parameters = {key_with_top_parameters, 3},
                                               .named_arity = 3,
                                               .result_type =
                                                   SXML_BYTE_VIEW_INITIALIZER("node()*")},
    [SXML_STYLESHEET_FUNCTION_COPY_OF] = {.local_name = SXML_BYTE_VIEW_INITIALIZER("copy-of"),
                                          .parameters = {copy_of_parameters, 1},
                                          .named_arity = 1,
                                          .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*")},
    [SXML_STYLESHEET_FUNCTION_SNAPSHOT] = {.local_name = SXML_BYTE_VIEW_INITIALIZER("snapshot"),
                                           .parameters = {copy_of_parameters, 1},
                                           .named_arity = 1,
                                           .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*")},
};

/* The most parameters of a stylesheet or private function. */
#define FUNCTION_MAXIMUM_PARAMETER_COUNT 3U

/* One function of the unit the tables answer: its types and, once an
   expression names it, its IR function. */
typedef struct table_function {
    sxml_name_identifier function_name;
    sxml_function_parameter_view parameters;
    sxml_sequence_type parameter_types[FUNCTION_MAXIMUM_PARAMETER_COUNT];
    sxml_sequence_type result_type;
    /* Of all the parameters. */
    sxml_type_identifier signature;
    /* Without the last parameter, for a name that passes it itself (fn:key#2,
       fn:copy-of#0); 0 otherwise. */
    sxml_type_identifier short_signature;
    sxml_ir_function_index function;
    bool is_lowered;
} table_function;

/* ------------------------------------------------------------------ Keys */

/* One xsl:key. */
typedef struct key_declaration {
    sxml_xslt_ast_node_index node;
    /* Its key in table->keys. */
    size_t key_index;
    /* The private functions match-N and use-N. */
    table_function match;
    table_function use;
} key_declaration;

/* The xsl:key declarations of one name (20.2.1). */
typedef struct key_definition {
    sxml_name_identifier key_name;
    /* The collation URI, borrowed from the AST; empty for the codepoint
       collation. */
    sxml_byte_view collation;
    bool is_composite;
    /* The private function build-K. */
    table_function build;
} key_definition;

/* ------------------------------------------------------------- The table */

struct sxml_xslt_function_table {
    sxml_xslt_lowering *lowering;
    sxml_allocator allocator;
    /* What the static contexts had: the processor's table. */
    sxml_function_table inner_table;
    /* The XSLT-only parts (D101) and the internal rows of keys. */
    const sxml_native_function_view **part_views;
    sxml_function_library *library;
    sxml_function_library *key_library;
    /* The stylesheet functions by kind. */
    table_function functions[SXML_STYLESHEET_FUNCTION_COUNT];
    key_declaration *declarations;
    size_t declaration_count;
    key_definition *keys;
    size_t key_count;
    bool are_keys_lowered;
    /* The static context of the texts made here: the base context with
       the private table. */
    sxml_xpath_static_context snippet_context;
    /* The texts' ASTs, kept until release (their spans are borrowed). */
    sxml_ast **snippets;
    size_t snippet_count;
    size_t snippet_capacity;
    /* Names of the texts' variables. */
    sxml_name_identifier
        variable_names[FUNCTION_MAXIMUM_PARAMETER_COUNT * SXML_STYLESHEET_FUNCTION_COUNT];
    sxml_namespace_uri_identifier private_namespace;
};

/* --------------------------------------------------------------- Helpers */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static const sxml_xslt_ast_attribute *find_attribute(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node,
                                                     sxml_xslt_ast_attribute_kind kind) {
    return sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(lowering->ast,
                                     (sxml_xslt_ast_attribute_query){.node = node, .kind = kind}));
}

static const sxml_xpath_static_context *
expression_context(const sxml_xslt_lowering *lowering, sxml_xslt_ast_expression_index expression) {
    const uint32_t context =
        sxml_xslt_ast_get_expression(lowering->ast, expression)->static_context.value;
    if (context == 0 || context >= lowering->static_context_count) {
        return NULL;
    }
    return &lowering->static_contexts[context];
}

/* An array of element_count (at least one) zeroed elements. */
typedef struct zeroed_request {
    size_t element_count;
    sxml_memory_requirements element;
} zeroed_request;

static sxml_status allocate_zeroed(sxml_allocator allocator, zeroed_request request,
                                   void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(request.element_count == 0 ? 1 : request.element_count,
                                            request.element.byte_count, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = request.element.alignment},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

static sxml_status report_static_error(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_ast_node_index node, sxml_byte_view text) {
    return sxml_xslt_lowering_report_static_error(
        lowering, (sxml_xslt_static_error){.node = node, .code_and_description = text});
}

/* UTF-8 text being made in the table's allocator. */
typedef struct snippet_text {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
    /* The first failure; later appends do nothing. */
    sxml_status status;
} snippet_text;

static void append_bytes(snippet_text *text, sxml_byte_view bytes) {
    if (text->status != SXML_STATUS_OK || bytes.byte_count == 0) {
        return;
    }
    size_t needed = 0;
    text->status = sxml_size_add(text->byte_count, bytes.byte_count, &needed);
    if (text->status == SXML_STATUS_OK && needed > text->byte_capacity) {
        size_t capacity = text->byte_capacity == 0 ? 256 : text->byte_capacity;
        while (capacity < needed && text->status == SXML_STATUS_OK) {
            text->status = sxml_size_multiply(capacity, 2, &capacity);
        }
        void *memory = NULL;
        if (text->status == SXML_STATUS_OK) {
            text->status = sxml_allocator_allocate(
                text->allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
                &memory);
        }
        if (text->status == SXML_STATUS_OK) {
            if (text->byte_count > 0) {
                memcpy(memory, text->bytes, text->byte_count);
            }
            sxml_allocator_release(text->allocator, text->bytes);
            text->bytes = memory;
            text->byte_capacity = capacity;
        }
    }
    if (text->status == SXML_STATUS_OK && text->bytes != NULL) {
        memcpy(text->bytes + text->byte_count, bytes.bytes, bytes.byte_count);
        text->byte_count = needed;
    }
}

/* Appends a string literal of C. */
#define APPEND_TEXT(text, literal) append_bytes((text), SXML_BYTE_VIEW_FROM_LITERAL(literal))

static void append_number(snippet_text *text, size_t number) {
    uint8_t digits[24];
    size_t digit_count = 0;
    size_t rest = number;
    do {
        digits[sizeof(digits) - 1 - digit_count] = (uint8_t)('0' + rest % 10U);
        digit_count += 1;
        rest /= 10U;
    } while (rest > 0 && digit_count < sizeof(digits));
    append_bytes(text, (sxml_byte_view){.bytes = digits + sizeof(digits) - digit_count,
                                        .byte_count = digit_count});
}

/* A string literal: double quotes, an embedded one doubled (XPath 4.0,
   A.2.1); ampersands need nothing in XPath. */
static void append_string_literal(snippet_text *text, sxml_byte_view value) {
    APPEND_TEXT(text, "\"");
    if (value.byte_count == 0) {
        APPEND_TEXT(text, "\"");
        return;
    }
    size_t start = 0;
    for (size_t byte_index = 0; byte_index < value.byte_count; byte_index += 1) {
        if (value.bytes[byte_index] == '"') {
            append_bytes(text, (sxml_byte_view){.bytes = value.bytes + start,
                                                .byte_count = byte_index + 1 - start});
            APPEND_TEXT(text, "\"");
            start = byte_index + 1;
        }
    }
    append_bytes(text, (sxml_byte_view){.bytes = value.bytes + start,
                                        .byte_count = value.byte_count - start});
    APPEND_TEXT(text, "\"");
}

static void release_text(snippet_text *text) {
    sxml_allocator_release(text->allocator, text->bytes);
    text->bytes = NULL;
    text->byte_count = 0;
    text->byte_capacity = 0;
}

static sxml_byte_view text_view(const snippet_text *text) {
    return (sxml_byte_view){.bytes = text->bytes, .byte_count = text->byte_count};
}

/* QName("uri", "local") for name. */
static void append_qname_constructor(const sxml_xslt_function_table *table, snippet_text *text,
                                     sxml_name_identifier name) {
    const sxml_name_pool *name_pool = table->lowering->lowerer->name_pool;
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (text->status == SXML_STATUS_OK) {
        text->status = sxml_name_pool_get_name_parts(name_pool, name, &parts);
    }
    if (text->status == SXML_STATUS_OK && parts.namespace_uri.index != 0) {
        text->status =
            sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri, &namespace_uri);
    }
    APPEND_TEXT(text, "QName(");
    append_string_literal(text, namespace_uri);
    APPEND_TEXT(text, ", ");
    append_string_literal(text, parts.local_name);
    APPEND_TEXT(text, ")");
}

/* --------------------------------------------------------- Function types */

/* Resolves a row-spelled type text with the XSLT library. */
static sxml_status resolve_type(const sxml_xslt_function_table *table, sxml_byte_view type_text,
                                sxml_sequence_type *sequence_type) {
    return sxml_function_library_resolve_type_text(table->library, type_text, sequence_type);
}

/* What a table function is: its name, parameters and result, and whether
   a name of one parameter fewer passes the last one. */
typedef struct table_function_shape {
    sxml_name_identifier function_name;
    sxml_function_parameter_view parameters;
    sxml_byte_view result_type;
    bool has_short_signature;
} table_function_shape;

static sxml_status prepare_function(const sxml_xslt_function_table *table,
                                    table_function_shape shape, table_function *function) {
    const size_t parameter_count = shape.parameters.function_parameter_count;
    if (parameter_count > FUNCTION_MAXIMUM_PARAMETER_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *function =
        (table_function){.function_name = shape.function_name, .parameters = shape.parameters};
    sxml_status status = SXML_STATUS_OK;
    for (size_t parameter_index = 0; status == SXML_STATUS_OK && parameter_index < parameter_count;
         parameter_index += 1) {
        status = resolve_type(table,
                              shape.parameters.function_parameters[parameter_index].parameter_type,
                              &function->parameter_types[parameter_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_type(table, shape.result_type, &function->result_type);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_register_signature(
            table->lowering->lowerer,
            (sxml_xpath_lowering_signature_request){
                .parameter_types = {.sequence_types = function->parameter_types,
                                    .sequence_type_count = parameter_count},
                .result_type = function->result_type},
            &function->signature);
    }
    if (status == SXML_STATUS_OK && shape.has_short_signature) {
        status = sxml_xpath_lowerer_register_signature(
            table->lowering->lowerer,
            (sxml_xpath_lowering_signature_request){
                .parameter_types = {.sequence_types = function->parameter_types,
                                    .sequence_type_count = parameter_count - 1},
                .result_type = function->result_type},
            &function->short_signature);
    }
    return status;
}

/* Adds function's IR function to the unit on first use. */
static sxml_status declare_function(const sxml_xslt_function_table *table,
                                    table_function *function) {
    if (function->function.value != 0) {
        return SXML_STATUS_OK;
    }
    return sxml_ir_unit_add_function(
        table->lowering->lowerer->unit,
        &(sxml_ir_function){.function_name = {.index = 0},
                            .signature = function->signature,
                            .parameter_count =
                                (uint32_t)function->parameters.function_parameter_count},
        &function->function);
}

/* The entry of function for a call or reference of arity. */
static void write_entry(const table_function *function, size_t arity, sxml_function_entry *entry) {
    const size_t parameter_count = function->parameters.function_parameter_count;
    *entry = (sxml_function_entry){
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION,
        .native_function = NULL,
        .ir_function_number = function->function.value,
        .parameters = function->parameters,
        .signature = arity < parameter_count ? function->short_signature : function->signature,
        /* fn:key#2 takes its third parameter, the context value, itself
           (src/function_library_internal.h, D149); fn:copy-of#0 has the
           types of arity 0, so the call goes to fn:copy-of#1. */
        .parameter_types = {.sequence_types = function->parameter_types,
                            .sequence_type_count = arity == 0 ? 0 : parameter_count},
        .result_type = function->result_type};
}

/* ------------------------------------------------------------- Lookups */

static bool is_function_name(const sxml_xslt_function_table *table, sxml_name_identifier name,
                             sxml_byte_view local_name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    return sxml_name_pool_get_name_parts(table->lowering->lowerer->name_pool, name, &parts) ==
               SXML_STATUS_OK &&
           parts.namespace_uri.index == SXML_NAMESPACE_FUNCTIONS &&
           sxml_byte_view_equals(parts.local_name, local_name);
}

/* The stylesheet function a query names, or SXML_STYLESHEET_FUNCTION_COUNT. */
static stylesheet_function_kind find_kind(const sxml_xslt_function_table *table,
                                          const sxml_function_query *query) {
    for (uint32_t kind_number = 0; kind_number < (uint32_t)SXML_STYLESHEET_FUNCTION_COUNT;
         kind_number += 1) {
        const stylesheet_function_rule *rule = &stylesheet_function_rules[kind_number];
        const bool arity_fits = query->arity == rule->named_arity ||
                                ((kind_number == (uint32_t)SXML_STYLESHEET_FUNCTION_COPY_OF ||
                                  kind_number == (uint32_t)SXML_STYLESHEET_FUNCTION_SNAPSHOT) &&
                                 query->arity == 0);
        if (arity_fits && is_function_name(table, query->function_name, rule->local_name)) {
            return (stylesheet_function_kind)kind_number;
        }
    }
    return SXML_STYLESHEET_FUNCTION_COUNT;
}

static sxml_status find_in_table(sxml_function_table table, const sxml_function_query *query,
                                 sxml_function_entry *entry) {
    *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    if (table.operations == NULL) {
        return SXML_STATUS_OK;
    }
    return sxml_function_table_find(table, query, entry);
}

/* The XSLT function table: stylesheet functions, the XSLT-only parts,
   then the table that was there. */
static sxml_status find_xslt_function(void *state, const sxml_function_query *query,
                                      sxml_function_entry *entry) {
    sxml_xslt_function_table *table = state;
    const stylesheet_function_kind kind = find_kind(table, query);
    if (kind != SXML_STYLESHEET_FUNCTION_COUNT) {
        table_function *function = &table->functions[kind];
        const sxml_status status = declare_function(table, function);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        write_entry(function, query->arity, entry);
        return SXML_STATUS_OK;
    }
    sxml_status status =
        find_in_table(sxml_function_library_as_table(table->library), query, entry);
    if (status != SXML_STATUS_OK || entry->entry_kind != SXML_FUNCTION_ENTRY_KIND_NONE) {
        return status;
    }
    return find_in_table(table->inner_table, query, entry);
}

static const sxml_function_table_operations xslt_function_table_operations = {
    .find_function = find_xslt_function};

/* The private functions of the key texts, then the internal rows, then
   the XSLT function table. */
static sxml_status find_private_function(void *state, const sxml_function_query *query,
                                         sxml_function_entry *entry) {
    sxml_xslt_function_table *table = state;
    if (query->arity == 1) {
        for (size_t declaration_index = 0; declaration_index < table->declaration_count;
             declaration_index += 1) {
            key_declaration *declaration = &table->declarations[declaration_index];
            table_function *candidates[2] = {&declaration->match, &declaration->use};
            for (size_t candidate_index = 0; candidate_index < 2; candidate_index += 1) {
                if (candidates[candidate_index]->function_name.index ==
                    query->function_name.index) {
                    write_entry(candidates[candidate_index], 1, entry);
                    return SXML_STATUS_OK;
                }
            }
        }
        for (size_t key_index = 0; key_index < table->key_count; key_index += 1) {
            if (table->keys[key_index].build.function_name.index == query->function_name.index) {
                write_entry(&table->keys[key_index].build, 1, entry);
                return SXML_STATUS_OK;
            }
        }
    }
    const sxml_status status =
        find_in_table(sxml_function_library_as_table(table->key_library), query, entry);
    if (status != SXML_STATUS_OK || entry->entry_kind != SXML_FUNCTION_ENTRY_KIND_NONE) {
        return status;
    }
    return find_xslt_function(state, query, entry);
}

static const sxml_function_table_operations private_function_table_operations = {
    .find_function = find_private_function};

/* ------------------------------------------------------------ Declaring */

static const sxml_function_parameter match_parameters[] = {
    SXML_FUNCTION_PARAMETER("item", "item()", SXML_PARAMETER_DEFAULT_REQUIRED),
};

static const sxml_function_parameter node_parameters[] = {
    SXML_FUNCTION_PARAMETER("node", "node()", SXML_PARAMETER_DEFAULT_REQUIRED),
};

/* Interns Q{KEY_FUNCTION_NAMESPACE_URI}prefix-number. */
typedef struct private_name_request {
    sxml_byte_view prefix;
    size_t number;
} private_name_request;

static sxml_status make_private_name(sxml_xslt_function_table *table, private_name_request request,
                                     sxml_name_identifier *name) {
    snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
    append_bytes(&text, request.prefix);
    append_number(&text, request.number);
    sxml_status status = text.status;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            table->lowering->lowerer->name_pool,
            &(sxml_name_request){.namespace_uri = table->private_namespace,
                                 .local_name = text_view(&text)},
            name);
    }
    release_text(&text);
    return status;
}

static sxml_byte_view collation_of(const sxml_xslt_lowering *lowering,
                                   sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *collation =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COLLATION);
    if (collation->value.kind == SXML_XSLT_AST_VALUE_KIND_TEXT) {
        return sxml_xslt_ast_get_text(lowering->ast, collation->value.payload.text);
    }
    /* 20.2.1: else the default collation of the declaration. */
    const sxml_xslt_ast_static_context *context =
        sxml_xslt_ast_get_static_context(lowering->ast, node_of(lowering, node)->static_context);
    return sxml_xslt_ast_get_text(lowering->ast, context->default_collation);
}

static bool is_composite(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *composite =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COMPOSITE);
    return composite->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
           composite->value.payload.is_true;
}

/* The key of name, added when new (20.2.1: XTSE1222 when declarations of
   one name differ in collation or composite). */
static sxml_status add_key_declaration(sxml_xslt_function_table *table,
                                       sxml_xslt_ast_node_index node) {
    const sxml_xslt_lowering *lowering = table->lowering;
    const sxml_xslt_ast_attribute *name =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_NAME);
    const sxml_xslt_ast_attribute *use =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_USE);
    const bool has_use = use->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION;
    const bool has_content =
        sxml_xslt_lowering_first_constructor_child(lowering->ast, node).value != 0;
    if (has_use == has_content) {
        return report_static_error(
            lowering, node,
            SXML_BYTE_VIEW_FROM_LITERAL("XTSE1205 an xsl:key has both a use attribute and a "
                                        "sequence constructor, or neither (20.2.1)"));
    }
    const sxml_byte_view collation = collation_of(lowering, node);
    const bool composite = is_composite(lowering, node);
    size_t key_index = 0;
    while (key_index < table->key_count &&
           table->keys[key_index].key_name.index != name->value.payload.name.index) {
        key_index += 1;
    }
    sxml_status status = SXML_STATUS_OK;
    if (key_index == table->key_count) {
        key_definition *key = &table->keys[key_index];
        *key = (key_definition){.key_name = name->value.payload.name,
                                .collation = collation,
                                .is_composite = composite};
        table->key_count += 1;
        sxml_name_identifier build_name = {.index = 0};
        status = make_private_name(
            table,
            (private_name_request){.prefix = SXML_BYTE_VIEW_FROM_LITERAL("build-"),
                                   .number = key_index + 1},
            &build_name);
        if (status == SXML_STATUS_OK) {
            status = prepare_function(
                table,
                (table_function_shape){.function_name = build_name,
                                       .parameters = {node_parameters, 1},
                                       .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)"),
                                       .has_short_signature = false},
                &key->build);
        }
    } else if (!sxml_byte_view_equals(table->keys[key_index].collation, collation) ||
               table->keys[key_index].is_composite != composite) {
        return report_static_error(
            lowering, node,
            SXML_BYTE_VIEW_FROM_LITERAL("XTSE1222 xsl:key declarations of one name differ in "
                                        "their collation or composite attribute (20.2.1)"));
    }
    const size_t declaration_number = table->declaration_count + 1;
    key_declaration *declaration = &table->declarations[table->declaration_count];
    *declaration = (key_declaration){.node = node, .key_index = key_index};
    table->declaration_count += 1;
    sxml_name_identifier match_name = {.index = 0};
    sxml_name_identifier use_name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = make_private_name(
            table,
            (private_name_request){.prefix = SXML_BYTE_VIEW_FROM_LITERAL("match-"),
                                   .number = declaration_number},
            &match_name);
    }
    if (status == SXML_STATUS_OK) {
        status =
            make_private_name(table,
                              (private_name_request){.prefix = SXML_BYTE_VIEW_FROM_LITERAL("use-"),
                                                     .number = declaration_number},
                              &use_name);
    }
    if (status == SXML_STATUS_OK) {
        status = prepare_function(
            table,
            (table_function_shape){.function_name = match_name,
                                   .parameters = {match_parameters, 1},
                                   .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
                                   .has_short_signature = false},
            &declaration->match);
    }
    if (status == SXML_STATUS_OK) {
        status = prepare_function(
            table,
            (table_function_shape){.function_name = use_name,
                                   .parameters = {node_parameters, 1},
                                   .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
                                   .has_short_signature = false},
            &declaration->use);
    }
    return status;
}

static sxml_status collect_keys(sxml_xslt_function_table *table) {
    const sxml_xslt_ast *ast = table->lowering->ast;
    size_t key_declaration_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        if (sxml_xslt_ast_get_node(ast, sxml_xslt_ast_get_declaration(ast, order))->kind ==
            SXML_XSLT_AST_KIND_KEY) {
            key_declaration_count += 1;
        }
    }
    void *memory = NULL;
    sxml_status status =
        allocate_zeroed(table->allocator,
                        (zeroed_request){.element_count = key_declaration_count,
                                         .element = {.byte_count = sizeof(key_declaration),
                                                     .alignment = alignof(key_declaration)}},
                        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->declarations = memory;
    status = allocate_zeroed(table->allocator,
                             (zeroed_request){.element_count = key_declaration_count,
                                              .element = {.byte_count = sizeof(key_definition),
                                                          .alignment = alignof(key_definition)}},
                             &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->keys = memory;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_ast_get_node(ast, node)->kind == SXML_XSLT_AST_KIND_KEY) {
            status = add_key_declaration(table, node);
        }
    }
    return status;
}

/* The library over the parts that serve XSLT and not XPath (D101): the
   processor's library has the others. */
static sxml_status make_libraries(sxml_xslt_function_table *table) {
    const sxml_function_library_part_view parts = sxml_function_library_registered_parts();
    size_t part_count = 0;
    for (size_t part_index = 0; part_index < parts.part_count; part_index += 1) {
        const sxml_language_set languages = parts.parts[part_index].languages;
        if ((languages & (sxml_language_set)SXML_LANGUAGE_XSLT) != 0 &&
            (languages & (sxml_language_set)SXML_LANGUAGE_XPATH) == 0) {
            part_count += 1;
        }
    }
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        table->allocator,
        (zeroed_request){.element_count = part_count,
                         .element = {.byte_count = sizeof(const sxml_native_function_view *),
                                     .alignment = alignof(const sxml_native_function_view *)}},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->part_views = (const sxml_native_function_view **)memory;
    size_t filled = 0;
    for (size_t part_index = 0; part_index < parts.part_count; part_index += 1) {
        const sxml_language_set languages = parts.parts[part_index].languages;
        if ((languages & (sxml_language_set)SXML_LANGUAGE_XSLT) != 0 &&
            (languages & (sxml_language_set)SXML_LANGUAGE_XPATH) == 0) {
            table->part_views[filled] = parts.parts[part_index].native_functions;
            filled += 1;
        }
    }
    sxml_xpath_lowerer *lowerer = table->lowering->lowerer;
    status = sxml_function_library_create(
        table->allocator,
        &(sxml_function_library_configuration){.allocator = table->allocator,
                                               .item_heap = lowerer->item_heap,
                                               .parts = {.native_function_views = table->part_views,
                                                         .native_function_view_count = part_count}},
        &table->library);
    static const sxml_native_function_view *const key_parts[] = {&sxml_xslt_key_functions};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_create(
            table->allocator,
            &(sxml_function_library_configuration){
                .allocator = table->allocator,
                .item_heap = lowerer->item_heap,
                .parts = {.native_function_views = key_parts, .native_function_view_count = 1}},
            &table->key_library);
    }
    return status;
}

static sxml_status prepare_stylesheet_functions(sxml_xslt_function_table *table) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t kind_number = 0;
         status == SXML_STATUS_OK && kind_number < (uint32_t)SXML_STYLESHEET_FUNCTION_COUNT;
         kind_number += 1) {
        const stylesheet_function_rule *rule = &stylesheet_function_rules[kind_number];
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            table->lowering->lowerer->name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS),
                                 .local_name = rule->local_name},
            &name);
        if (status == SXML_STATUS_OK) {
            status = prepare_function(table,
                                      (table_function_shape){.function_name = name,
                                                             .parameters = rule->parameters,
                                                             .result_type = rule->result_type,
                                                             .has_short_signature = true},
                                      &table->functions[kind_number]);
        }
    }
    static const char *const variable_names[] = {"key-name", "key-value", "context",
                                                 "top",      "root",      "input"};
    for (size_t name_index = 0; status == SXML_STATUS_OK &&
                                name_index < sizeof(variable_names) / sizeof(variable_names[0]);
         name_index += 1) {
        status = sxml_name_pool_intern_name(
            table->lowering->lowerer->name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name =
                                     sxml_byte_view_from_c_string(variable_names[name_index])},
            &table->variable_names[name_index]);
    }
    return status;
}

/* The indices of table->variable_names. */
typedef enum snippet_variable {
    SXML_SNIPPET_VARIABLE_KEY_NAME,
    SXML_SNIPPET_VARIABLE_KEY_VALUE,
    SXML_SNIPPET_VARIABLE_CONTEXT,
    SXML_SNIPPET_VARIABLE_TOP,
    SXML_SNIPPET_VARIABLE_ROOT,
    SXML_SNIPPET_VARIABLE_INPUT,
} snippet_variable;

sxml_status sxml_xslt_declare_functions(sxml_xslt_lowering *lowering,
                                        sxml_xpath_static_context *static_contexts,
                                        sxml_xslt_function_table **table) {
    *table = NULL;
    const sxml_allocator allocator = lowering->lowerer->allocator;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        allocator,
        (zeroed_request){.element_count = 1,
                         .element = {.byte_count = sizeof(sxml_xslt_function_table),
                                     .alignment = alignof(sxml_xslt_function_table)}},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_function_table *made = memory;
    made->lowering = lowering;
    made->allocator = allocator;
    made->inner_table = lowering->lowerer->static_context->function_table;
    made->snippet_context = *lowering->lowerer->static_context;
    made->snippet_context.namespace_bindings =
        (sxml_namespace_binding_view){.namespace_bindings = NULL, .namespace_binding_count = 0};
    made->snippet_context.variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = NULL, .variable_declaration_count = 0};
    made->snippet_context.function_table =
        (sxml_function_table){.state = made, .operations = &private_function_table_operations};
    status = sxml_name_pool_intern_namespace_uri(
        lowering->lowerer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(KEY_FUNCTION_NAMESPACE_URI),
        &made->private_namespace);
    if (status == SXML_STATUS_OK) {
        status = make_libraries(made);
    }
    if (status == SXML_STATUS_OK) {
        status = prepare_stylesheet_functions(made);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_keys(made);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xslt_release_functions(made);
        return status;
    }
    for (uint32_t context = 1; context < lowering->static_context_count; context += 1) {
        static_contexts[context].function_table =
            (sxml_function_table){.state = made, .operations = &xslt_function_table_operations};
    }
    *table = made;
    return SXML_STATUS_OK;
}

void sxml_xslt_release_functions(sxml_xslt_function_table *table) {
    if (table == NULL) {
        return;
    }
    for (size_t snippet_index = 0; snippet_index < table->snippet_count; snippet_index += 1) {
        sxml_ast_destroy(table->snippets[snippet_index]);
    }
    sxml_allocator_release(table->allocator, (void *)table->snippets);
    sxml_function_library_destroy(table->key_library);
    sxml_function_library_destroy(table->library);
    sxml_allocator_release(table->allocator, (void *)table->part_views);
    sxml_allocator_release(table->allocator, table->declarations);
    sxml_allocator_release(table->allocator, table->keys);
    sxml_allocator_release(table->allocator, table);
}

/* ------------------------------------------------------------- Lowering */

static sxml_ir_atom slot_atom(uint32_t slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = {.value = slot}};
}

/* Parses text and keeps the AST until release. */
static sxml_status parse_snippet(sxml_xslt_function_table *table, sxml_byte_view text,
                                 sxml_ast **ast) {
    if (table->snippet_count == table->snippet_capacity) {
        const size_t capacity = table->snippet_capacity == 0 ? 8 : table->snippet_capacity * 2;
        void *memory = NULL;
        sxml_status status =
            allocate_zeroed(table->allocator,
                            (zeroed_request){.element_count = capacity,
                                             .element = {.byte_count = sizeof(sxml_ast *),
                                                         .alignment = alignof(sxml_ast *)}},
                            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (table->snippet_count > 0) {
            memcpy(memory, (const void *)table->snippets,
                   table->snippet_count * sizeof(sxml_ast *));
        }
        sxml_allocator_release(table->allocator, (void *)table->snippets);
        table->snippets = (sxml_ast **)memory;
        table->snippet_capacity = capacity;
    }
    const sxml_status status = sxml_xpath_build_ast(
        &(sxml_ast_build_request){.expression_text = text,
                                  .source_name = {.bytes = NULL, .byte_count = 0},
                                  .allocator = table->allocator,
                                  .maximum_expression_depth = 0,
                                  .error_record = table->lowering->error_record},
        ast);
    if (status == SXML_STATUS_OK) {
        table->snippets[table->snippet_count] = *ast;
        table->snippet_count += 1;
    }
    return status;
}

/* A body for a table function's IR function, after its frame is left
   (whose slot count it takes). */
typedef struct function_body {
    const table_function *function;
    const sxml_xpath_lowering_function_frame *frame;
    sxml_ir_expression_index body;
} function_body;

static sxml_status replace_body(const sxml_xslt_function_table *table, function_body request) {
    sxml_xpath_lowerer *lowerer = table->lowering->lowerer;
    sxml_ir_function_replacement replacement = {
        .index = request.function->function,
        .function = *sxml_ir_unit_get_function(lowerer->unit, request.function->function)};
    replacement.function.body = request.body;
    replacement.function.slot_count = request.frame->function.slot_count;
    return sxml_ir_unit_replace_function(lowerer->unit, &replacement);
}

/* A function whose body is a text made here, its parameters bound to the
   variables named by variables (indices of table->variable_names). */
typedef struct snippet_function {
    const table_function *function;
    sxml_byte_view text;
    const snippet_variable *variables;
    /* At least the function's parameter count. */
    size_t variable_count;
    /* The value's nodes are copies of the items of the first parameter
       that keep their accumulator values (fn:snapshot, XSLT 4.0, 19.10). */
    bool links_copies;
} snippet_function;

static sxml_status lower_snippet_function(sxml_xslt_function_table *table,
                                          snippet_function request) {
    sxml_xpath_lowerer *lowerer = table->lowering->lowerer;
    sxml_ast *ast = NULL;
    sxml_status status = parse_snippet(table, request.text, &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t parameter_count =
        (uint32_t)request.function->parameters.function_parameter_count;
    if (request.variable_count < parameter_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = request.function->function,
                     .function_level = 0,
                     .slot_count = parameter_count,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &frame);
    for (uint32_t parameter = 0; status == SXML_STATUS_OK && parameter < parameter_count;
         parameter += 1) {
        status = sxml_xpath_lowerer_push_variable(
            lowerer, (sxml_xpath_lowering_scope_entry){
                         .variable_name = table->variable_names[request.variables[parameter]],
                         .function_level = 0,
                         .atom = slot_atom(parameter + 1)});
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_lower_embedded(
            lowerer,
            &(sxml_xpath_lowering_embedding){.ast = ast,
                                             .node = {.value = 0},
                                             .static_context = &table->snippet_context,
                                             .focus = NULL},
            &body);
    }
    if (status == SXML_STATUS_OK && request.links_copies &&
        sxml_xslt_has_accumulators(table->lowering)) {
        sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
        sxml_ir_atom copies = {0};
        status = sxml_xpath_lowerer_bind_expression(
            lowerer, (sxml_xpath_lowering_expression_in_block){.block = &block, .expression = body},
            &copies);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_bind_accumulator_link(
                lowerer,
                (sxml_xslt_accumulator_link_request){
                    .block = &block, .copies = copies, .originals = slot_atom(1)},
                &copies);
        }
        sxml_ir_expression_index result = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_expression(
                lowerer,
                (sxml_xpath_lowering_expression_to_add){
                    .node = no_ast_node,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                                   .atom = copies}},
                &result);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result},
                &body);
        }
    }
    sxml_xpath_lowerer_leave_function(lowerer, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return replace_body(
        table, (function_body){.function = request.function, .frame = &frame, .body = body});
}

/* match-N: the pattern's match function inside a TRY_CATCH whose one
   clause catches every error and gives false (6.5). */
static sxml_status lower_match_wrapper(sxml_xslt_function_table *table,
                                       key_declaration *declaration) {
    const sxml_xslt_lowering *lowering = table->lowering;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    const sxml_xslt_ast_attribute *match =
        find_attribute(lowering, declaration->node, SXML_XSLT_AST_ATTRIBUTE_MATCH);
    if (match->value.kind != SXML_XSLT_AST_VALUE_KIND_PATTERN) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xslt_ast_expression *pattern =
        sxml_xslt_ast_get_expression(lowering->ast, match->value.payload.expression);
    sxml_ir_function_index pattern_function = {0};
    sxml_status status = sxml_xslt_pattern_lower_match_function(
        lowerer,
        &(sxml_xslt_pattern_match_function_request){
            .pattern_ast = pattern->ast,
            .static_context = expression_context(lowering, match->value.payload.expression)},
        &pattern_function);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = declaration->match.function,
                     .function_level = 0,
                     .slot_count = 1,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &frame);
    const sxml_ir_atom item = slot_atom(1);
    sxml_ir_atom_list arguments = {0};
    status = sxml_ir_unit_add_atoms(
        lowerer->unit, (sxml_ir_atom_view){.atoms = &item, .atom_count = 1}, &arguments);
    sxml_ir_expression_index call = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression =
                    {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                     .static_call = {.target = {.target_kind =
                                                    SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                                .function = pattern_function},
                                     .arguments = arguments}}},
            &call);
    }
    const sxml_name_test any_error = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME};
    sxml_ir_catch_clause clause = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_name_tests(
            lowerer->unit, (sxml_ir_name_test_view){.name_tests = &any_error, .name_test_count = 1},
            &clause.error_tests);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(lowerer, &clause.error_details);
    }
    sxml_ir_atom false_atom = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(
            lowerer,
            sxml_function_library_make_boolean((sxml_boolean_result){.is_true = false}).content,
            &false_atom);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                               .atom = false_atom}},
            &clause.handler);
    }
    sxml_ir_catch_clause_list clauses = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_catch_clauses(
            lowerer->unit,
            (sxml_ir_catch_clause_view){.catch_clauses = &clause, .catch_clause_count = 1},
            &clauses);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                               .try_catch = {.body = call,
                                             .catch_clauses = clauses,
                                             .finally_body = {.value = 0}}}},
            &body);
    }
    sxml_xpath_lowerer_leave_function(lowerer, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return replace_body(
        table, (function_body){.function = &declaration->match, .frame = &frame, .body = body});
}

/* The sequence constructor of an xsl:key as a body, in a machine. */
typedef struct key_content_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} key_content_locals;

static sxml_status continue_key_content(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    key_content_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = locals->head.lowering,
                         .first = sxml_xslt_lowering_first_constructor_child(
                             locals->head.lowering->ast, locals->head.node),
                         .block = frame->block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &locals->value,
                                         .atom_block = frame->block,
                                         .focused = NULL},
                         .focus = NULL});
    }
    sxml_ir_expression_index result = {0};
    const sxml_status status = sxml_xpath_lowerer_add_expression(
        machine->state,
        (sxml_xpath_lowering_expression_to_add){
            .node = no_ast_node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = locals->value}},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static const sxml_xpath_lowering_procedure key_content_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:key"),
    .step = continue_key_content,
    .locals_size = sizeof(key_content_locals)};

/* use-N: the use attribute or the sequence constructor with the node as
   its fixed focus (20.2.1). */
static sxml_status lower_use_function(sxml_xslt_function_table *table,
                                      const key_declaration *declaration) {
    sxml_xslt_lowering *lowering = table->lowering;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = declaration->use.function,
                     .function_level = 0,
                     .slot_count = 1,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED, .fixed_value = slot_atom(1)},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &frame);
    const sxml_xslt_ast_attribute *use =
        find_attribute(lowering, declaration->node, SXML_XSLT_AST_ATTRIBUTE_USE);
    sxml_ir_expression_index body = {0};
    sxml_status status = SXML_STATUS_OK;
    if (use->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        status = sxml_xpath_lowerer_lower_embedded(
            lowerer,
            &(sxml_xpath_lowering_embedding){
                .ast =
                    sxml_xslt_ast_get_expression(lowering->ast, use->value.payload.expression)->ast,
                .node = {.value = 0},
                .static_context = expression_context(lowering, use->value.payload.expression),
                .focus = NULL},
            &body);
    } else {
        sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
        const sxml_xslt_instruction_locals initial = {.lowering = lowering,
                                                      .node = declaration->node};
        status = sxml_xpath_lowerer_run_procedure(
            lowerer, &(sxml_xpath_lowering_procedure_call){
                         .procedure = &key_content_procedure,
                         .node = no_ast_node,
                         .block = &block,
                         .is_body = false,
                         .destination = {.expression = &body,
                                         .atom = NULL,
                                         .atom_block = NULL,
                                         .focused = NULL},
                         .initial_locals = {.bytes = (const uint8_t *)&initial,
                                            .byte_count = sizeof(initial)}});
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = body}, &body);
        }
    }
    sxml_xpath_lowerer_leave_function(lowerer, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return replace_body(
        table, (function_body){.function = &declaration->use, .frame = &frame, .body = body});
}

/* The text of build-K (see the file comment). */
static void write_build_text(const sxml_xslt_function_table *table, size_t key_index,
                             snippet_text *text) {
    const key_definition *key = &table->keys[key_index];
    APPEND_TEXT(text, "Q{http://www.w3.org/2005/xpath-functions/map}build("
                      "($root/descendant-or-self::node() | $root/descendant-or-self::*/@*), "
                      "fn($node) { ()");
    for (size_t declaration_index = 0; declaration_index < table->declaration_count;
         declaration_index += 1) {
        if (table->declarations[declaration_index].key_index != key_index) {
            continue;
        }
        APPEND_TEXT(text, ", (if (Q{" KEY_FUNCTION_NAMESPACE_URI "}match-");
        append_number(text, declaration_index + 1);
        APPEND_TEXT(text, "($node)) then Q{}key-normalize(Q{" KEY_FUNCTION_NAMESPACE_URI "}use-");
        append_number(text, declaration_index + 1);
        APPEND_TEXT(text, "($node), ");
        append_string_literal(text, key->collation);
        append_bytes(text, key->is_composite ? SXML_BYTE_VIEW_FROM_LITERAL(", true())")
                                             : SXML_BYTE_VIEW_FROM_LITERAL(", false())"));
        APPEND_TEXT(text, " else ())");
    }
    APPEND_TEXT(text, " })");
}

/* The private functions and build functions of every key, once. */
static sxml_status lower_keys(sxml_xslt_function_table *table) {
    if (table->are_keys_lowered) {
        return SXML_STATUS_OK;
    }
    table->are_keys_lowered = true;
    sxml_status status = SXML_STATUS_OK;
    for (size_t declaration_index = 0;
         status == SXML_STATUS_OK && declaration_index < table->declaration_count;
         declaration_index += 1) {
        key_declaration *declaration = &table->declarations[declaration_index];
        status = declare_function(table, &declaration->match);
        if (status == SXML_STATUS_OK) {
            status = declare_function(table, &declaration->use);
        }
    }
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < table->key_count;
         key_index += 1) {
        status = declare_function(table, &table->keys[key_index].build);
    }
    for (size_t declaration_index = 0;
         status == SXML_STATUS_OK && declaration_index < table->declaration_count;
         declaration_index += 1) {
        status = lower_match_wrapper(table, &table->declarations[declaration_index]);
        if (status == SXML_STATUS_OK) {
            status = lower_use_function(table, &table->declarations[declaration_index]);
        }
    }
    static const snippet_variable build_variables[] = {SXML_SNIPPET_VARIABLE_ROOT};
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < table->key_count;
         key_index += 1) {
        snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
        write_build_text(table, key_index, &text);
        status = text.status;
        if (status == SXML_STATUS_OK) {
            status = lower_snippet_function(
                table, (snippet_function){.function = &table->keys[key_index].build,
                                          .text = text_view(&text),
                                          .variables = build_variables,
                                          .variable_count = 1});
        }
        release_text(&text);
    }
    return status;
}

/* The text of fn:key#2 (KEY_WITH_CONTEXT) or fn:key#3 (see the file
   comment). */
static void write_key_text(const sxml_xslt_function_table *table, stylesheet_function_kind kind,
                           snippet_text *text) {
    const bool with_context = kind == SXML_STYLESHEET_FUNCTION_KEY_WITH_CONTEXT;
    APPEND_TEXT(text, "let $name := Q{}key-name($key-name), $root := root(");
    append_bytes(text, with_context ? SXML_BYTE_VIEW_FROM_LITERAL("$context")
                                    : SXML_BYTE_VIEW_FROM_LITERAL("$top"));
    APPEND_TEXT(text, ") return ");
    if (with_context) {
        /* 20.2.2: XTDE1270 when the root of the context node's tree is not
           a document node. */
        APPEND_TEXT(text, "if (not($root instance of document-node())) then "
                          "error(QName(\"http://www.w3.org/2005/xqt-errors\", \"XTDE1270\"), "
                          "\"fn:key with two arguments is called where the root of the tree of the "
                          "context node is not a document node (XSLT 4.0, 20.2.2)\") else ");
    }
    APPEND_TEXT(text, "(");
    for (size_t key_index = 0; key_index < table->key_count; key_index += 1) {
        const key_definition *key = &table->keys[key_index];
        APPEND_TEXT(text, "if ($name eq ");
        append_qname_constructor(table, text, key->key_name);
        APPEND_TEXT(text, ") then (let $index := Q{}key-index(");
        append_number(text, key_index + 1);
        APPEND_TEXT(text, ", $root, Q{" KEY_FUNCTION_NAMESPACE_URI "}build-");
        append_number(text, key_index + 1);
        APPEND_TEXT(text, "#1) return for $value in Q{}key-normalize($key-value, ");
        append_string_literal(text, key->collation);
        append_bytes(text, key->is_composite ? SXML_BYTE_VIEW_FROM_LITERAL(", true())")
                                             : SXML_BYTE_VIEW_FROM_LITERAL(", false())"));
        APPEND_TEXT(text, " return $index($value)) else ");
    }
    APPEND_TEXT(text, "error(QName(\"http://www.w3.org/2005/xqt-errors\", \"XTDE1260\"), "
                      "\"no xsl:key declares the name given to fn:key (XSLT 4.0, 20.2.2)\"))");
    if (!with_context) {
        APPEND_TEXT(text, "[exists(ancestor-or-self::node()[. is $top])]");
    }
    APPEND_TEXT(text, "/.");
}

static sxml_status lower_key_function(sxml_xslt_function_table *table,
                                      stylesheet_function_kind kind) {
    const bool with_context = kind == SXML_STYLESHEET_FUNCTION_KEY_WITH_CONTEXT;
    sxml_status status = lower_keys(table);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    static const snippet_variable context_variables[] = {SXML_SNIPPET_VARIABLE_KEY_NAME,
                                                         SXML_SNIPPET_VARIABLE_KEY_VALUE,
                                                         SXML_SNIPPET_VARIABLE_CONTEXT};
    static const snippet_variable top_variables[] = {
        SXML_SNIPPET_VARIABLE_KEY_NAME, SXML_SNIPPET_VARIABLE_KEY_VALUE, SXML_SNIPPET_VARIABLE_TOP};
    snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
    write_key_text(table, kind, &text);
    status = text.status;
    if (status == SXML_STATUS_OK) {
        status = lower_snippet_function(
            table,
            (snippet_function){
                .function =
                    &table->functions[with_context ? SXML_STYLESHEET_FUNCTION_KEY_WITH_CONTEXT
                                                   : SXML_STYLESHEET_FUNCTION_KEY_WITH_TOP],
                .text = text_view(&text),
                .variables = with_context ? context_variables : top_variables,
                .variable_count = 3});
    }
    release_text(&text);
    return status;
}

/* fn:copy-of($input) (XSLT 4.0, 20.6.1): the deep copies xsl:copy-of
   makes (11.9.2, copy-namespaces="yes"). */
static sxml_status lower_copy_of(sxml_xslt_function_table *table) {
    sxml_xpath_lowerer *lowerer = table->lowering->lowerer;
    const table_function *function = &table->functions[SXML_STYLESHEET_FUNCTION_COPY_OF];
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = function->function,
                     .function_level = 0,
                     .slot_count = 1,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &frame);
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom copies = {0};
    sxml_status status = sxml_xslt_lowering_bind_copy_of(
        lowerer,
        (sxml_xslt_copy_request){.block = &block,
                                 .value = slot_atom(1),
                                 .construction_flags = (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES},
        &copies);
    if (status == SXML_STATUS_OK && sxml_xslt_has_accumulators(table->lowering)) {
        /* XSLT 4.0, 19.10: the copies keep the accumulator values (packet
           4.14). */
        status = sxml_xslt_bind_accumulator_link(
            lowerer,
            (sxml_xslt_accumulator_link_request){
                .block = &block, .copies = copies, .originals = slot_atom(1)},
            &copies);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = copies}},
            &result);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result}, &body);
    }
    sxml_xpath_lowerer_leave_function(lowerer, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return replace_body(table,
                        (function_body){.function = function, .frame = &frame, .body = body});
}

/* fn:snapshot($input) (XSLT 4.0, 20.6.2): a node is copied with copies of
   its ancestors, their attributes and namespaces, and the copy of the node
   in that new tree is returned; other items are copied as fn:copy-of
   copies them. The ancestors are wrapped from the innermost out
   (fold-right), then the copy is found again level by level. */
static const char snapshot_text[] =
    "for $item in $input return "
    "if (not($item instance of node()) or empty($item/..)) then copy-of($item) "
    "else let $ancestors := $item/ancestor::node(), "
    "$leaf := if ($item instance of attribute()) then $item else copy-of($item), "
    "$tree := fold-right($ancestors, $leaf, fn($ancestor, $inner) { "
    "if ($ancestor instance of document-node()) then document { $inner } "
    "else element { node-name($ancestor) } { $ancestor/namespace::*, $ancestor/@*[not(. is "
    "$item)], $inner } }), "
    "$parent := fold-left(2 to count($ancestors), $tree, fn($node, $level) { $node/node() }) "
    "return if ($item instance of attribute()) "
    "then $parent/@*[node-name(.) eq node-name($item)] else $parent/node()";

static sxml_status lower_snapshot(sxml_xslt_function_table *table) {
    static const snippet_variable input_variables[] = {SXML_SNIPPET_VARIABLE_INPUT};
    return lower_snippet_function(
        table, (snippet_function){.function = &table->functions[SXML_STYLESHEET_FUNCTION_SNAPSHOT],
                                  .text = {.bytes = (const uint8_t *)snapshot_text,
                                           .byte_count = sizeof(snapshot_text) - 1},
                                  .variables = input_variables,
                                  .variable_count = 1,
                                  .links_copies = true});
}

sxml_status sxml_xslt_lower_functions(sxml_xslt_function_table *table) {
    if (table == NULL) {
        return SXML_STATUS_OK;
    }
    /* Each body may name another stylesheet function: until none is left
       unlowered (at most one round per kind). */
    sxml_status status = SXML_STATUS_OK;
    bool has_lowered = true;
    while (status == SXML_STATUS_OK && has_lowered) {
        has_lowered = false;
        for (uint32_t kind_number = 0;
             status == SXML_STATUS_OK && kind_number < (uint32_t)SXML_STYLESHEET_FUNCTION_COUNT;
             kind_number += 1) {
            table_function *function = &table->functions[kind_number];
            if (function->function.value == 0 || function->is_lowered) {
                continue;
            }
            function->is_lowered = true;
            has_lowered = true;
            switch ((stylesheet_function_kind)kind_number) {
            case SXML_STYLESHEET_FUNCTION_KEY_WITH_CONTEXT:
                status = lower_key_function(table, SXML_STYLESHEET_FUNCTION_KEY_WITH_CONTEXT);
                break;
            case SXML_STYLESHEET_FUNCTION_KEY_WITH_TOP:
                status = lower_key_function(table, SXML_STYLESHEET_FUNCTION_KEY_WITH_TOP);
                break;
            case SXML_STYLESHEET_FUNCTION_COPY_OF:
                status = lower_copy_of(table);
                break;
            case SXML_STYLESHEET_FUNCTION_SNAPSHOT:
                status = lower_snapshot(table);
                break;
            case SXML_STYLESHEET_FUNCTION_COUNT:
                break;
            }
        }
    }
    return status;
}
