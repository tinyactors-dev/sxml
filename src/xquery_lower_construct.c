/*
 * The XQuery 4.0 lowering part `construct` (src/xpath_lower_internal.h;
 * docs/DECISIONS.md, D55, D103, D110, D124; packet 3.9): direct and
 * computed node constructors. Computed constructors are XPath 4.0 too
 * (XPath 4.0, section 4.13: "Computed node constructors are now available
 * in XPath as well as XQuery"), so the part serves every language; the
 * direct constructors only parse in XQuery.
 *
 * Golden file: src/xpath_lower.c (procedures as frames whose steps ask the
 * machine for their parts and return, D71; continue_atom_list for a list
 * of atoms in owned memory).
 *
 * Every constructor lowers to one IR constructor of its kind (src/
 * ir_internal.h, "The node constructor kinds") whose operands are atoms
 * bound in the caller's block:
 *
 * - A direct element constructor (4.12.1) has content of kind SEQUENCE
 *   with one atom per attribute and per piece of content, in order (D110:
 *   atomic items are joined by spaces only within one atom, so
 *   `<c>{1}{2}</c>` is "12"). Literal text and CDATA sections are xs:string
 *   constants; boundary whitespace (4.12.1.4) is dropped unless the prolog
 *   declares boundary-space preserve. Its name is an xs:QName constant
 *   resolved while lowering (XPST0081), with its prefix.
 * - Namespace declaration attributes (4.12.1.2) make no attribute: they
 *   are the element's namespace bindings, and they extend the statically
 *   known namespaces (and with "xmlns" the default element namespace) of
 *   the element's own names and of everything inside it. The element's
 *   attributes and content are then lowered through the embedding entry
 *   with a static context whose namespace bindings are the declarations
 *   followed by the enclosing ones, so the core resolves names inside
 *   enclosed expressions with them. Each element, direct or computed, gets
 *   as namespace bindings the declarations of its own and enclosing
 *   direct constructors (innermost first, one per prefix), which XQuery
 *   puts among its in-scope namespaces whatever the copy-namespaces mode;
 *   a computed name given as a string resolves with the extended
 *   namespaces (name_namespaces, D124).
 * - A direct attribute's value (4.12.1.1): "" for no parts, the constant
 *   for literal text alone, the enclosed expression's atom alone (the
 *   evaluator joins its atomized items with spaces), and otherwise the
 *   parts joined by "||", each enclosed expression as a text constructor
 *   of its value (an empty value gives no text node, "" for "||"). Two
 *   attributes of one element with the same expanded name are XQST0040.
 * - Computed constructors (4.12.3) take a name constant (an xs:QName for
 *   element and attribute names, an xs:string for a PI target or a
 *   namespace prefix) or the atom of the name expression, and the atom of
 *   the content expression: the value of attribute, namespace, text,
 *   comment and PI constructors, an ATOM content expression of element and
 *   document constructors (one enclosed expression, D110).
 * - Direct comments and PIs (4.12.2) are constructors of constants.
 *
 * The prolog's boundary-space, construction and copy-namespaces
 * declarations (5.3, 5.6, 5.9) are read from the module's PROLOG; the last
 * two become construction_flags of element and document constructors.
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026).
 */
#include "ir_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"
#include "xpath_lower_internal.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ----------------------------------------------------------- AST access */

static const sxml_ast_node *node_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_get_node(state->ast, node);
}

static sxml_ast_node_index child_of(const sxml_xpath_lowerer *state, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(state->ast,
                               (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(state->ast, node);
}

static sxml_byte_view text_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_get_text(state->ast, node_of(state, node)->payload.text);
}

static bool has_flag(const sxml_xpath_lowerer *state, sxml_ast_node_index node, uint32_t flag) {
    return (node_of(state, node)->flags & flag) != 0;
}

/* ------------------------------------------------------------ The prolog */

/* The first declaration of kind in the prolog of the lowerer's module; 0
   when there is none (or the AST is not a module). */
static sxml_ast_node_index find_prolog_declaration(const sxml_xpath_lowerer *state,
                                                   sxml_ast_kind kind) {
    const sxml_ast_node_index none = {.value = 0};
    const sxml_ast_node_index root = state->ast->root;
    if (node_of(state, root)->kind != SXML_AST_KIND_MODULE) {
        return none;
    }
    const sxml_ast_node_index module = child_of(state, root, SXML_AST_FIELD_MODULE);
    if (module.value == 0) {
        return none;
    }
    const sxml_ast_node_index prolog = child_of(state, module, SXML_AST_FIELD_PROLOG);
    if (prolog.value == 0) {
        return none;
    }
    for (sxml_ast_node_index declaration = child_of(state, prolog, SXML_AST_FIELD_DECLARATION);
         declaration.value != 0; declaration = next_of(state, declaration)) {
        if (node_of(state, declaration)->kind == kind) {
            return declaration;
        }
    }
    return none;
}

/* Boundary-space preserve (5.3); strip is the default. */
static bool preserves_boundary_space(const sxml_xpath_lowerer *state) {
    const sxml_ast_node_index declaration =
        find_prolog_declaration(state, SXML_AST_KIND_BOUNDARY_SPACE_DECLARATION);
    return declaration.value != 0 &&
           node_of(state, declaration)->detail.keyword == SXML_AST_KEYWORD_PRESERVE;
}

/* The construction_flags of element and document constructors: the
   construction mode (5.6) and the copy-namespaces modes (5.9); 0 for the
   defaults strip, preserve, inherit. */
static uint32_t construction_flags_of(const sxml_xpath_lowerer *state) {
    uint32_t flags = 0;
    const sxml_ast_node_index construction =
        find_prolog_declaration(state, SXML_AST_KIND_CONSTRUCTION_DECLARATION);
    if (construction.value != 0 &&
        node_of(state, construction)->detail.keyword == SXML_AST_KEYWORD_PRESERVE) {
        flags |= (uint32_t)SXML_IR_CONSTRUCTION_PRESERVES_TYPES;
    }
    const sxml_ast_node_index copy_namespaces =
        find_prolog_declaration(state, SXML_AST_KIND_COPY_NAMESPACES_DECLARATION);
    if (copy_namespaces.value != 0) {
        if (!has_flag(state, copy_namespaces, SXML_AST_FLAG_PRESERVE_NAMESPACES)) {
            flags |= (uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES;
        }
        if (!has_flag(state, copy_namespaces, SXML_AST_FLAG_INHERIT_NAMESPACES)) {
            flags |= (uint32_t)SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES;
        }
    }
    return flags;
}

/* --------------------------------------------------------- Small pieces */

/* Adds expression and binds it in block. */
typedef struct bound_expression {
    sxml_ast_node_index node;
    sxml_xpath_lowering_block *block;
    sxml_ir_expression expression;
} bound_expression;

static sxml_status bind_new_expression(sxml_xpath_lowerer *state, const bound_expression *request,
                                       sxml_ir_atom *atom) {
    sxml_ir_expression_index index = {0};
    const sxml_status status = sxml_xpath_lowerer_add_expression(
        state,
        (sxml_xpath_lowering_expression_to_add){.node = request->node,
                                                .expression = request->expression},
        &index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = request->block, .expression = index},
        atom);
}

/* A name node of the lowerer's AST resolved by role, as an xs:QName
   constant that keeps the prefix written. */
typedef struct qname_constant_request {
    sxml_ast_node_index name_node;
    sxml_xpath_name_role name_role;
    /* Where names resolve: the lowerer's static context or an element's
       extended one. */
    const sxml_xpath_static_context *static_context;
} qname_constant_request;

/* The expanded name and prefix of a name node. */
typedef struct resolved_qname {
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
} resolved_qname;

static sxml_status resolve_qname(sxml_xpath_lowerer *state, const qname_constant_request *request,
                                 resolved_qname *qname) {
    sxml_xpath_resolved_name resolved = {.namespace_uri = {0}, .local_name = {0}};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = state->ast,
                                   .name_node = request->name_node,
                                   .name_role = request->name_role,
                                   .static_context = request->static_context,
                                   .error_record = state->error_record},
        &resolved);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_name_pool_intern_name(state->name_pool,
                                       &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                                            .local_name = resolved.local_name},
                                       &qname->expanded_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            state->name_pool,
            sxml_ast_get_text(state->ast,
                              node_of(state, request->name_node)->payload.name.namespace_prefix),
            &qname->namespace_prefix);
    }
    return status;
}

static sxml_status add_qname_constant(sxml_xpath_lowerer *state,
                                      const qname_constant_request *request, sxml_ir_atom *atom) {
    resolved_qname qname = {.expanded_name = {0}, .namespace_prefix = {0}};
    sxml_status status = resolve_qname(state, request, &qname);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            state->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = qname.expanded_name,
                                                      .namespace_prefix =
                                                          qname.namespace_prefix}}}},
            &item);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(state, item, atom);
}

/* The name node of a computed constructor's literal name: the Name itself,
   or the name inside a QName literal ("#a") or marked NCName. */
static sxml_ast_node_index literal_name_node(const sxml_xpath_lowerer *state,
                                             sxml_ast_node_index name) {
    const sxml_ast_kind kind = node_of(state, name)->kind;
    if (kind == SXML_AST_KIND_QNAME_LITERAL || kind == SXML_AST_KIND_MARKED_NCNAME) {
        return child_of(state, name, SXML_AST_FIELD_NAME);
    }
    return name;
}

/* --------------------------------------------- Namespaces of elements */

/* XML whitespace (XML 1.0, production S). */
static bool is_xml_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* The URI of a namespace declaration attribute: its literal text (one
   part, or none for ""), with leading and trailing whitespace removed
   (4.12.1.2: the value is whitespace-normalized as an xs:anyURI). */
static sxml_byte_view declared_uri_text(const sxml_xpath_lowerer *state,
                                        sxml_ast_node_index attribute) {
    const sxml_ast_node_index value = child_of(state, attribute, SXML_AST_FIELD_VALUE);
    const sxml_ast_node_index part = child_of(state, value, SXML_AST_FIELD_PART);
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (part.value != 0) {
        text = text_of(state, part);
    }
    while (text.byte_count > 0 && is_xml_whitespace(text.bytes[0])) {
        text.bytes += 1;
        text.byte_count -= 1;
    }
    while (text.byte_count > 0 && is_xml_whitespace(text.bytes[text.byte_count - 1])) {
        text.byte_count -= 1;
    }
    return text;
}

/* The binding a namespace declaration attribute makes: "xmlns" binds
   prefix 0 (the default element namespace; "" undeclares it), "xmlns:p"
   binds p. */
static sxml_status declared_binding(sxml_xpath_lowerer *state, sxml_ast_node_index attribute,
                                    sxml_namespace_binding *binding) {
    const sxml_ast_node_index name = child_of(state, attribute, SXML_AST_FIELD_NAME);
    const sxml_ast_name written = node_of(state, name)->payload.name;
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    if (written.namespace_prefix.byte_count > 0) {
        prefix = sxml_ast_get_text(state->ast, written.local_name);
    }
    sxml_status status = sxml_name_pool_intern_namespace_prefix(state->name_pool, prefix,
                                                                &binding->namespace_prefix);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            state->name_pool, declared_uri_text(state, attribute), &binding->namespace_uri);
    }
    return status;
}

static bool view_binds(sxml_namespace_binding_view bindings,
                       sxml_namespace_prefix_identifier prefix) {
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        if (bindings.namespace_bindings[binding_index].namespace_prefix.index == prefix.index) {
            return true;
        }
    }
    return false;
}

/* Appends to bindings[*count] the entries of more whose prefix is not yet
   bound there. */
typedef struct binding_append {
    sxml_namespace_binding *bindings;
    sxml_namespace_binding_view more;
} binding_append;

static void append_unbound(binding_append request, size_t *count) {
    for (size_t binding_index = 0; binding_index < request.more.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding = &request.more.namespace_bindings[binding_index];
        if (!view_binds((sxml_namespace_binding_view){.namespace_bindings = request.bindings,
                                                      .namespace_binding_count = *count},
                        binding->namespace_prefix)) {
            request.bindings[*count] = *binding;
            *count += 1;
        }
    }
}

/* ------------------------------------------------ Direct elements */

/* "<a ...>...</a>" (4.12.1). */
typedef struct direct_element_locals {
    /* The next attribute, then the next content child, to lower. */
    sxml_ast_node_index attribute;
    sxml_ast_node_index content;
    /* One atom per attribute and piece of content (owned memory 0). */
    sxml_ir_atom *atoms;
    size_t atom_count;
    size_t atom_capacity;
    sxml_ir_atom name;
    /* The declarations of this and the enclosing direct constructors,
       one per prefix, innermost first (owned memory 1, or the enclosing
       element's), and their run in the unit. */
    sxml_namespace_binding_view declared;
    sxml_ir_namespace_binding_list declared_list;
    /* The static context of the attributes and content when the element
       declares namespaces (owned memory 2); NULL otherwise. */
    sxml_xpath_static_context *own_context;
    bool preserves_boundary_space;
} direct_element_locals;

static sxml_status continue_direct_element(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame);

static const sxml_xpath_lowering_procedure direct_element_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery direct element constructor"),
    .step = continue_direct_element,
    .locals_size = sizeof(direct_element_locals)};

/* The locals of the nearest direct element constructor being lowered
   below frame; NULL outside one. */
static const direct_element_locals *find_enclosing_element(const sxml_xpath_lowering_frame *frame) {
    for (const sxml_xpath_lowering_frame *below = frame->below; below != NULL;
         below = below->below) {
        if (below->procedure == &direct_element_procedure) {
            return below->locals;
        }
    }
    return NULL;
}

/* The namespace bindings an element constructor sends: the declarations
   of the enclosing direct constructors; none outside one. */
static sxml_ir_namespace_binding_list
enclosing_declared_list(const sxml_xpath_lowering_frame *frame) {
    const direct_element_locals *enclosing = find_enclosing_element(frame);
    if (enclosing == NULL) {
        return (sxml_ir_namespace_binding_list){.first_namespace_binding_index = 0,
                                                .namespace_binding_count = 0};
    }
    return enclosing->declared_list;
}

/* Counts the element's namespace declaration attributes. */
static size_t count_declarations(const sxml_xpath_lowerer *state, sxml_ast_node_index element) {
    size_t declaration_count = 0;
    for (sxml_ast_node_index attribute = child_of(state, element, SXML_AST_FIELD_ATTRIBUTE);
         attribute.value != 0; attribute = next_of(state, attribute)) {
        if (has_flag(state, attribute, SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE)) {
            declaration_count += 1;
        }
    }
    return declaration_count;
}

/* Makes the element's declared bindings and its static context: own
   declarations first (a binding of "xml" is the predeclared one and is not
   repeated), then the enclosing declarations, and for the context the
   lowerer's bindings after them. Memory 1 holds both binding arrays. */
static sxml_status declare_namespaces(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame, size_t declaration_count) {
    sxml_xpath_lowerer *state = machine->state;
    direct_element_locals *locals = frame->locals;
    const direct_element_locals *enclosing = find_enclosing_element(frame);
    const sxml_namespace_binding_view enclosing_declared =
        enclosing == NULL ? (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                                          .namespace_binding_count = 0}
                          : enclosing->declared;
    const sxml_namespace_binding_view outer = state->static_context->namespace_bindings;
    const size_t declared_capacity = declaration_count + enclosing_declared.namespace_binding_count;
    const size_t context_capacity = declaration_count + outer.namespace_binding_count;
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 1,
            .scratch = {.element_count = declared_capacity + context_capacity,
                        .element_size = sizeof(sxml_namespace_binding),
                        .element_alignment = alignof(sxml_namespace_binding)}});
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 2,
                .scratch = {.element_count = 1,
                            .element_size = sizeof(sxml_xpath_static_context),
                            .element_alignment = alignof(sxml_xpath_static_context)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_namespace_binding *declared = frame->owned_memory[1];
    sxml_namespace_binding *context_bindings = declared + declared_capacity;
    sxml_namespace_prefix_identifier xml_prefix = {.index = 0};
    status = sxml_name_pool_intern_namespace_prefix(
        state->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xml"), &xml_prefix);
    size_t own_count = 0;
    for (sxml_ast_node_index attribute = child_of(state, frame->node, SXML_AST_FIELD_ATTRIBUTE);
         status == SXML_STATUS_OK && attribute.value != 0; attribute = next_of(state, attribute)) {
        if (!has_flag(state, attribute, SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE)) {
            continue;
        }
        sxml_namespace_binding binding = {.namespace_prefix = {0}, .namespace_uri = {0}};
        status = declared_binding(state, attribute, &binding);
        if (status == SXML_STATUS_OK && binding.namespace_prefix.index != xml_prefix.index) {
            declared[own_count] = binding;
            own_count += 1;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(context_bindings, declared, own_count * sizeof(sxml_namespace_binding));
    size_t declared_count = own_count;
    append_unbound((binding_append){.bindings = declared, .more = enclosing_declared},
                   &declared_count);
    size_t context_count = own_count;
    append_unbound((binding_append){.bindings = context_bindings, .more = outer}, &context_count);
    locals->declared = (sxml_namespace_binding_view){.namespace_bindings = declared,
                                                     .namespace_binding_count = declared_count};
    status =
        sxml_ir_unit_add_namespace_bindings(state->unit, locals->declared, &locals->declared_list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->own_context = frame->owned_memory[2];
    *locals->own_context = *state->static_context;
    locals->own_context->namespace_bindings = (sxml_namespace_binding_view){
        .namespace_bindings = context_bindings, .namespace_binding_count = context_count};
    return SXML_STATUS_OK;
}

/* XQST0040: two attributes with one expanded name (4.12.1, the same name
   written differently; the front end catches the same name written
   alike). */
static sxml_status check_attribute_names(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame, size_t attribute_count) {
    sxml_xpath_lowerer *state = machine->state;
    const direct_element_locals *locals = frame->locals;
    if (attribute_count < 2) {
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 3,
            .scratch = {.element_count = attribute_count,
                        .element_size = sizeof(sxml_name_identifier),
                        .element_alignment = alignof(sxml_name_identifier)}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_identifier *names = frame->owned_memory[3];
    const sxml_xpath_static_context *context =
        locals->own_context != NULL ? locals->own_context : state->static_context;
    size_t name_count = 0;
    for (sxml_ast_node_index attribute = child_of(state, frame->node, SXML_AST_FIELD_ATTRIBUTE);
         attribute.value != 0; attribute = next_of(state, attribute)) {
        if (has_flag(state, attribute, SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE)) {
            continue;
        }
        resolved_qname qname = {.expanded_name = {0}, .namespace_prefix = {0}};
        status = resolve_qname(
            state,
            &(qname_constant_request){.name_node = child_of(state, attribute, SXML_AST_FIELD_NAME),
                                      .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                      .static_context = context},
            &qname);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        for (size_t earlier_index = 0; earlier_index < name_count; earlier_index += 1) {
            if (names[earlier_index].index == qname.expanded_name.index) {
                return sxml_xpath_lowerer_report_static_error(
                    state, attribute,
                    SXML_BYTE_VIEW_FROM_LITERAL(
                        "XQST0040 two attributes of a direct element constructor have "
                        "the same expanded name"));
            }
        }
        names[name_count] = qname.expanded_name;
        name_count += 1;
    }
    return SXML_STATUS_OK;
}

/* The first step: namespaces, the name, the attribute check and the atom
   array. */
static sxml_status start_direct_element(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    direct_element_locals *locals = frame->locals;
    const sxml_ast_node_index element = frame->node;
    locals->preserves_boundary_space = preserves_boundary_space(state);
    const size_t declaration_count = count_declarations(state, element);
    sxml_status status = SXML_STATUS_OK;
    if (declaration_count > 0) {
        status = declare_namespaces(machine, frame, declaration_count);
    } else {
        const direct_element_locals *enclosing = find_enclosing_element(frame);
        if (enclosing != NULL) {
            locals->declared = enclosing->declared;
            locals->declared_list = enclosing->declared_list;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = add_qname_constant(
            state,
            &(qname_constant_request){.name_node = child_of(state, element, SXML_AST_FIELD_NAME),
                                      .name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
                                      .static_context = locals->own_context != NULL
                                                            ? locals->own_context
                                                            : state->static_context},
            &locals->name);
    }
    size_t attribute_count = 0;
    for (sxml_ast_node_index attribute = child_of(state, element, SXML_AST_FIELD_ATTRIBUTE);
         attribute.value != 0; attribute = next_of(state, attribute)) {
        attribute_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = check_attribute_names(machine, frame, attribute_count - declaration_count);
    }
    size_t content_count = 0;
    for (sxml_ast_node_index content = child_of(state, element, SXML_AST_FIELD_CONTENT);
         content.value != 0; content = next_of(state, content)) {
        content_count += 1;
    }
    locals->atom_capacity = attribute_count - declaration_count + content_count;
    if (status == SXML_STATUS_OK && locals->atom_capacity > 0) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = locals->atom_capacity,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        locals->atoms = frame->owned_memory[0];
    }
    locals->attribute = child_of(state, element, SXML_AST_FIELD_ATTRIBUTE);
    locals->content = child_of(state, element, SXML_AST_FIELD_CONTENT);
    return status;
}

/* The next attribute or piece of content to lower to an atom; 0 when all
   are done. Literal text is made a constant at once. */
static sxml_status next_element_part(sxml_xpath_lowerer *state, direct_element_locals *locals,
                                     sxml_ast_node_index *part) {
    *part = (sxml_ast_node_index){.value = 0};
    while (locals->attribute.value != 0) {
        const sxml_ast_node_index attribute = locals->attribute;
        locals->attribute = next_of(state, attribute);
        if (!has_flag(state, attribute, SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE)) {
            *part = attribute;
            return SXML_STATUS_OK;
        }
    }
    while (locals->content.value != 0) {
        const sxml_ast_node_index content = locals->content;
        locals->content = next_of(state, content);
        const sxml_ast_kind kind = node_of(state, content)->kind;
        if (kind == SXML_AST_KIND_DIRECT_ELEMENT_TEXT &&
            has_flag(state, content, SXML_AST_FLAG_BOUNDARY_WHITESPACE) &&
            !locals->preserves_boundary_space) {
            continue;
        }
        if (kind == SXML_AST_KIND_DIRECT_ELEMENT_TEXT || kind == SXML_AST_KIND_CDATA_SECTION) {
            const sxml_status status = sxml_xpath_lowerer_add_string_constant(
                state, text_of(state, content), &locals->atoms[locals->atom_count]);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            locals->atom_count += 1;
            continue;
        }
        *part = content;
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* Lowers part to the next atom: through the embedding entry with the
   element's own static context when it declares namespaces. */
static sxml_status lower_element_part(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame, sxml_ast_node_index part) {
    sxml_xpath_lowerer *state = machine->state;
    direct_element_locals *locals = frame->locals;
    sxml_ir_atom *atom = &locals->atoms[locals->atom_count];
    locals->atom_count += 1;
    if (locals->own_context == NULL) {
        return sxml_xpath_lowering_call_atom(
            machine,
            (sxml_xpath_lowering_atom_call){.node = part, .block = frame->block, .atom = atom});
    }
    return sxml_xpath_lowering_call_embedded(
        machine,
        &(sxml_xpath_lowering_embedded_call){
            .embedding = {.ast = state->ast,
                          .node = part,
                          .static_context = locals->own_context,
                          .focus = NULL},
            .block = frame->block,
            .is_body = false,
            .destination = {
                .expression = NULL, .atom = atom, .atom_block = frame->block, .focused = NULL}});
}

static sxml_status finish_direct_element(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    const direct_element_locals *locals = frame->locals;
    sxml_ir_atom_list atoms = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        state->unit, (sxml_ir_atom_view){.atoms = locals->atoms, .atom_count = locals->atom_count},
        &atoms);
    sxml_ir_expression_index content = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = frame->node,
                .expression =
                    (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                         .sequence = atoms}},
            &content);
    }
    sxml_ir_expression_index element = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = frame->node,
                .expression =
                    (sxml_ir_expression){
                        .expression_kind = SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
                        .node_construction = {.name = locals->name,
                                              .content = content,
                                              .namespace_bindings = locals->declared_list,
                                              .construction_flags = construction_flags_of(state)}}},
            &element);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, element);
}

static sxml_status continue_direct_element(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    direct_element_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_status status = start_direct_element(machine, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_ast_node_index part = {.value = 0};
    const sxml_status status = next_element_part(state, locals, &part);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (part.value != 0) {
        return lower_element_part(machine, frame, part);
    }
    return finish_direct_element(machine, frame);
}

/* ----------------------------------------------- Direct attributes */

/* One attribute of a direct element constructor (4.12.1.1); lowered only
   as a part of its element. */
typedef struct direct_attribute_locals {
    sxml_ir_atom name;
    /* The next part of the value; the atom of the part being lowered. */
    sxml_ast_node_index part;
    sxml_ast_node_index current_part;
    sxml_ir_atom part_value;
    /* The value so far. */
    sxml_ir_atom value;
    bool has_value;
    /* One enclosed expression and nothing else: its atom is the value. */
    bool is_single_enclosed;
} direct_attribute_locals;

/* Appends an atom to the attribute's value with "||". */
static sxml_status append_value(sxml_xpath_lowerer *state, sxml_xpath_lowering_frame *frame,
                                sxml_ir_atom part_atom) {
    direct_attribute_locals *locals = frame->locals;
    if (!locals->has_value) {
        locals->value = part_atom;
        locals->has_value = true;
        return SXML_STATUS_OK;
    }
    return bind_new_expression(
        state,
        &(bound_expression){
            .node = locals->current_part,
            .block = frame->block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                           .binary = {.operator= SXML_IR_BINARY_OPERATOR_CONCATENATE,
                                      .left_operand = locals->value,
                                      .right_operand = part_atom}}},
        &locals->value);
}

/* After an enclosed part: alone, its atom is the value; among other
   parts, a text constructor of it is appended. */
static sxml_status append_enclosed_value(sxml_xpath_lowerer *state,
                                         sxml_xpath_lowering_frame *frame) {
    direct_attribute_locals *locals = frame->locals;
    if (locals->is_single_enclosed) {
        locals->value = locals->part_value;
        locals->has_value = true;
        return SXML_STATUS_OK;
    }
    sxml_ir_atom text = {0};
    const sxml_status status = bind_new_expression(
        state,
        &(bound_expression){
            .node = locals->current_part,
            .block = frame->block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                           .node_construction = {.value = locals->part_value}}},
        &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return append_value(state, frame, text);
}

static sxml_status continue_direct_attribute(sxml_xpath_lowering_machine *machine,
                                             sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    direct_attribute_locals *locals = frame->locals;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        frame->step = 1;
        status =
            add_qname_constant(state,
                               &(qname_constant_request){
                                   .name_node = child_of(state, frame->node, SXML_AST_FIELD_NAME),
                                   .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                   .static_context = state->static_context},
                               &locals->name);
        const sxml_ast_node_index value = child_of(state, frame->node, SXML_AST_FIELD_VALUE);
        locals->part = child_of(state, value, SXML_AST_FIELD_PART);
        locals->is_single_enclosed =
            locals->part.value != 0 && next_of(state, locals->part).value == 0 &&
            node_of(state, locals->part)->kind == SXML_AST_KIND_ENCLOSED_EXPRESSION;
    } else {
        status = append_enclosed_value(state, frame);
    }
    while (status == SXML_STATUS_OK && locals->part.value != 0) {
        const sxml_ast_node_index part = locals->part;
        locals->part = next_of(state, part);
        locals->current_part = part;
        if (node_of(state, part)->kind == SXML_AST_KIND_ENCLOSED_EXPRESSION) {
            return sxml_xpath_lowering_call_atom(
                machine, (sxml_xpath_lowering_atom_call){
                             .node = part, .block = frame->block, .atom = &locals->part_value});
        }
        sxml_ir_atom text = {0};
        status = sxml_xpath_lowerer_add_string_constant(state, text_of(state, part), &text);
        if (status == SXML_STATUS_OK) {
            status = append_value(state, frame, text);
        }
    }
    if (status == SXML_STATUS_OK && !locals->has_value) {
        status = sxml_xpath_lowerer_add_string_constant(
            state, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &locals->value);
    }
    sxml_ir_expression_index attribute = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = frame->node,
                .expression =
                    (sxml_ir_expression){
                        .expression_kind = SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                        .node_construction = {.name = locals->name, .value = locals->value}}},
            &attribute);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, attribute);
}

static const sxml_xpath_lowering_procedure direct_attribute_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery direct attribute"),
    .step = continue_direct_attribute,
    .locals_size = sizeof(direct_attribute_locals)};

/* ------------------------------------- Direct comments and PIs (4.12.2) */

static sxml_status lower_direct_comment(sxml_xpath_lowerer *state,
                                        sxml_xpath_lowering_node_in_block request,
                                        sxml_ir_expression_index *expression) {
    sxml_ir_atom value = {0};
    const sxml_status status =
        sxml_xpath_lowerer_add_string_constant(state, text_of(state, request.node), &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_expression(
        state,
        (sxml_xpath_lowering_expression_to_add){
            .node = request.node,
            .expression =
                (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
                                     .node_construction = {.value = value}}},
        expression);
}

static sxml_status lower_direct_processing_instruction(sxml_xpath_lowerer *state,
                                                       sxml_xpath_lowering_node_in_block request,
                                                       sxml_ir_expression_index *expression) {
    const sxml_ast_processing_instruction written =
        node_of(state, request.node)->payload.processing_instruction;
    sxml_ir_atom target = {0};
    sxml_ir_atom value = {0};
    sxml_status status = sxml_xpath_lowerer_add_string_constant(
        state, sxml_ast_get_text(state->ast, written.target), &target);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_string_constant(
            state, sxml_ast_get_text(state->ast, written.content), &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_expression(
        state,
        (sxml_xpath_lowering_expression_to_add){
            .node = request.node,
            .expression =
                (sxml_ir_expression){.expression_kind =
                                         SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                                     .node_construction = {.name = target, .value = value}}},
        expression);
}

/* ------------------------------------------- Computed constructors */

/* "element N {E}", "attribute N {E}", "namespace N {E}",
   "processing-instruction N {E}", "document {E}", "text {E}",
   "comment {E}" (4.12.3). */
typedef struct computed_locals {
    sxml_ir_atom name;
    sxml_ir_atom content;
} computed_locals;

static sxml_ir_expression_kind constructor_kind(sxml_ast_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_COMPUTED_DOCUMENT_CONSTRUCTOR:
        return SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR;
    case SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR:
        return SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR;
    case SXML_AST_KIND_COMPUTED_ATTRIBUTE_CONSTRUCTOR:
        return SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR;
    case SXML_AST_KIND_COMPUTED_NAMESPACE_CONSTRUCTOR:
        return SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR;
    case SXML_AST_KIND_COMPUTED_PROCESSING_INSTRUCTION_CONSTRUCTOR:
        return SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR;
    case SXML_AST_KIND_COMPUTED_TEXT_CONSTRUCTOR:
        return SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR;
    default:
        return SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR;
    }
}

/* The constant of a literal name (4.12.3.1 to 4.12.3.5): an xs:QName for
   elements (default element namespace) and attributes (no namespace), an
   xs:string NCName for PI targets and namespace prefixes. */
static sxml_status add_literal_name(sxml_xpath_lowerer *state, sxml_ast_node_index constructor,
                                    sxml_ir_atom *atom) {
    const sxml_ast_kind kind = node_of(state, constructor)->kind;
    const sxml_ast_node_index name =
        literal_name_node(state, child_of(state, constructor, SXML_AST_FIELD_NAME));
    if (kind == SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR ||
        kind == SXML_AST_KIND_COMPUTED_ATTRIBUTE_CONSTRUCTOR) {
        return add_qname_constant(
            state,
            &(qname_constant_request){.name_node = name,
                                      .name_role =
                                          kind == SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR
                                              ? SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE
                                              : SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                      .static_context = state->static_context},
            atom);
    }
    return sxml_xpath_lowerer_add_string_constant(
        state, sxml_ast_get_text(state->ast, node_of(state, name)->payload.name.local_name), atom);
}

/* The name namespaces of a constructor whose name is computed: the
   lowerer's static namespaces inside a direct constructor that extends
   them; empty (the unit's) otherwise. */
static sxml_status add_name_namespaces(sxml_xpath_lowerer *state,
                                       const sxml_xpath_lowering_frame *frame,
                                       sxml_ir_namespace_binding_list *list) {
    const direct_element_locals *enclosing = find_enclosing_element(frame);
    *list = (sxml_ir_namespace_binding_list){.first_namespace_binding_index = 0,
                                             .namespace_binding_count = 0};
    if (enclosing == NULL || enclosing->declared.namespace_binding_count == 0 ||
        state->static_context->namespace_bindings.namespace_binding_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_ir_unit_add_namespace_bindings(state->unit,
                                               state->static_context->namespace_bindings, list);
}

static sxml_status finish_computed(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    const computed_locals *locals = frame->locals;
    const sxml_ir_expression_kind kind = constructor_kind(node_of(state, frame->node)->kind);
    sxml_ir_node_construction construction = {.name = locals->name};
    sxml_status status = SXML_STATUS_OK;
    if (kind == SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR ||
        kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = frame->node,
                .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                                                   .atom = locals->content}},
            &construction.content);
        construction.construction_flags = construction_flags_of(state);
    } else {
        construction.value = locals->content;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR) {
        construction.namespace_bindings = enclosing_declared_list(frame);
    }
    const bool has_name_expression =
        child_of(state, frame->node, SXML_AST_FIELD_NAME_EXPRESSION).value != 0;
    if (status == SXML_STATUS_OK && has_name_expression &&
        (kind == SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR ||
         kind == SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR)) {
        status = add_name_namespaces(state, frame, &construction.name_namespaces);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = frame->node,
                .expression = (sxml_ir_expression){.expression_kind = kind,
                                                   .node_construction = construction}},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

/* Lowers the content expression to an atom, or finishes without one. */
static sxml_status lower_computed_content(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    computed_locals *locals = frame->locals;
    frame->step = 2;
    const sxml_ast_node_index content = child_of(state, frame->node, SXML_AST_FIELD_CONTENT);
    if (content.value == 0) {
        return finish_computed(machine, frame);
    }
    return sxml_xpath_lowering_call_atom(
        machine, (sxml_xpath_lowering_atom_call){
                     .node = content, .block = frame->block, .atom = &locals->content});
}

static sxml_status continue_computed(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    computed_locals *locals = frame->locals;
    if (frame->step == 1) {
        return lower_computed_content(machine, frame);
    }
    if (frame->step == 2) {
        return finish_computed(machine, frame);
    }
    frame->step = 1;
    const sxml_ast_node_index name_expression =
        child_of(state, frame->node, SXML_AST_FIELD_NAME_EXPRESSION);
    if (name_expression.value != 0) {
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = name_expression, .block = frame->block, .atom = &locals->name});
    }
    if (child_of(state, frame->node, SXML_AST_FIELD_NAME).value != 0) {
        const sxml_status status = add_literal_name(state, frame->node, &locals->name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return lower_computed_content(machine, frame);
}

static const sxml_xpath_lowering_procedure computed_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery computed constructor"),
    .step = continue_computed,
    .locals_size = sizeof(computed_locals)};

/* ================================================================= Part */

static const sxml_xpath_lowering_rule construct_rules[] = {
    {.kind = SXML_AST_KIND_DIRECT_ELEMENT_CONSTRUCTOR, .procedure = &direct_element_procedure},
    {.kind = SXML_AST_KIND_DIRECT_ATTRIBUTE, .procedure = &direct_attribute_procedure},
    {.kind = SXML_AST_KIND_DIRECT_COMMENT_CONSTRUCTOR, .leaf = lower_direct_comment},
    {.kind = SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR,
     .leaf = lower_direct_processing_instruction},
    {.kind = SXML_AST_KIND_COMPUTED_DOCUMENT_CONSTRUCTOR, .procedure = &computed_procedure},
    {.kind = SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR, .procedure = &computed_procedure},
    {.kind = SXML_AST_KIND_COMPUTED_ATTRIBUTE_CONSTRUCTOR, .procedure = &computed_procedure},
    {.kind = SXML_AST_KIND_COMPUTED_NAMESPACE_CONSTRUCTOR, .procedure = &computed_procedure},
    {.kind = SXML_AST_KIND_COMPUTED_PROCESSING_INSTRUCTION_CONSTRUCTOR,
     .procedure = &computed_procedure},
    {.kind = SXML_AST_KIND_COMPUTED_TEXT_CONSTRUCTOR, .procedure = &computed_procedure},
    {.kind = SXML_AST_KIND_COMPUTED_COMMENT_CONSTRUCTOR, .procedure = &computed_procedure},
};

const sxml_xpath_lowering_part sxml_xpath_lowering_part_construct = {
    .languages = SXML_LANGUAGE_SET_ALL,
    .rules = construct_rules,
    .rule_count = sizeof(construct_rules) / sizeof(construct_rules[0])};
