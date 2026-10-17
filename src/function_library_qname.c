/*
 * The QName part of the function library (packet 1.23b, D91): the parts of
 * a QName (F&O 4.0, section 10.2: fn:local-name-from-QName,
 * fn:prefix-from-QName, fn:namespace-uri-from-QName, fn:expanded-QName)
 * and the in-scope namespaces of an element (F&O 4.0, section 10.2:
 * fn:in-scope-prefixes, fn:in-scope-namespaces,
 * fn:namespace-uri-for-prefix); making QNames from strings (F&O 4.0,
 * section 10.1: fn:parse-QName, fn:resolve-QName; packet 2.11).
 *
 * Golden file: src/function_library_node.c (names through the name pool,
 * atomic results); rows as in src/function_library_internal.h.
 *
 * The in-scope namespaces come from the tree (sxml_node_get_in_scope_
 * namespaces): without undeclarations and with the xml prefix, as
 * dm:namespace-nodes gives them (XDM 4.0, section 7.5.2.2).
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "ir_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>

/* ----------------------------------------------------------------- Items */

/* Makes an atomic item of a string type (xs:NCName, xs:anyURI, xs:string)
   and writes it, owned, as a singleton to *result. */
typedef struct string_item_request {
    sxml_builtin_type type;
    sxml_byte_view text;
} string_item_request;

static sxml_status make_string_item(sxml_item_heap *item_heap, string_item_request request,
                                    sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(request.type),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = request.text,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* The parts of a QName argument. */
typedef struct qname_parts {
    bool has_value;
    sxml_byte_view local_name;
    sxml_byte_view namespace_uri;
    sxml_byte_view prefix;
} qname_parts;

/* Reads argument 0, of type xs:QName?, into *parts. */
static sxml_status read_qname(const sxml_function_call *call, qname_parts *parts) {
    *parts = (qname_parts){.has_value = false};
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(argument.item, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_QNAME) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_name_parts name_parts;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(
            name_pool, datum.datum_details.qname.expanded_name, &name_parts);
    }
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(name_pool, name_parts.namespace_uri,
                                                  &namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_prefix(
            name_pool, datum.datum_details.qname.namespace_prefix, &prefix);
    }
    if (status == SXML_STATUS_OK) {
        *parts = (qname_parts){.has_value = true,
                               .local_name = name_parts.local_name,
                               .namespace_uri = namespace_uri,
                               .prefix = prefix};
    }
    return status;
}

/* fn:local-name-from-QName($value as xs:QName?) as xs:NCName? (F&O 4.0,
   section 10.2.3). */
static sxml_status call_local_name_from_qname(const sxml_function_call *call,
                                              sxml_sequence *result) {
    qname_parts parts;
    const sxml_status status = read_qname(call, &parts);
    if (status != SXML_STATUS_OK || !parts.has_value) {
        *result = sxml_sequence_empty();
        return status;
    }
    return make_string_item(
        call->item_heap, (string_item_request){.type = SXML_TYPE_XS_NCNAME, .text = parts.local_name},
        result);
}

/* fn:prefix-from-QName($value as xs:QName?) as xs:NCName? (F&O 4.0,
   section 10.2.2): the empty sequence when the QName has no prefix. */
static sxml_status call_prefix_from_qname(const sxml_function_call *call,
                                          sxml_sequence *result) {
    qname_parts parts;
    const sxml_status status = read_qname(call, &parts);
    if (status != SXML_STATUS_OK || !parts.has_value || parts.prefix.byte_count == 0) {
        *result = sxml_sequence_empty();
        return status;
    }
    return make_string_item(
        call->item_heap, (string_item_request){.type = SXML_TYPE_XS_NCNAME, .text = parts.prefix},
        result);
}

/* fn:namespace-uri-from-QName($value as xs:QName?) as xs:anyURI? (F&O 4.0,
   section 10.2.4): the zero-length xs:anyURI for a name in no
   namespace. */
static sxml_status call_namespace_uri_from_qname(const sxml_function_call *call,
                                                 sxml_sequence *result) {
    qname_parts parts;
    const sxml_status status = read_qname(call, &parts);
    if (status != SXML_STATUS_OK || !parts.has_value) {
        *result = sxml_sequence_empty();
        return status;
    }
    return make_string_item(
        call->item_heap,
        (string_item_request){.type = SXML_TYPE_XS_ANY_URI, .text = parts.namespace_uri}, result);
}

/* fn:expanded-QName($value as xs:QName?) as xs:string? (F&O 4.0, section
   10.2.5): the URI-qualified name Q{uri}local, Q{}local in no
   namespace. */
static sxml_status call_expanded_qname(const sxml_function_call *call,
                                       sxml_sequence *result) {
    qname_parts parts;
    sxml_status status = read_qname(call, &parts);
    if (status != SXML_STATUS_OK || !parts.has_value) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, parts.namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("}"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, parts.local_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* --------------------------------------------------- In-scope namespaces */

/* Reads the element argument at argument_index (coerced to element()) and
   writes its in-scope namespaces, owned by the tree, to *bindings. */
typedef struct element_namespaces {
    sxml_namespace_binding_view bindings;
} element_namespaces;

static sxml_status read_in_scope_namespaces(const sxml_function_call *call,
                                            size_t argument_index,
                                            element_namespaces *namespaces) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &argument);
    if (status == SXML_STATUS_OK && !argument.has_item) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_node_reference element;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(argument.item, &element);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_in_scope_namespaces(element, &namespaces->bindings);
    }
    return status;
}

/* The texts of one binding: its prefix ("" for the default namespace) and
   its URI. */
typedef struct binding_texts {
    sxml_byte_view prefix;
    sxml_byte_view namespace_uri;
} binding_texts;

static sxml_status read_binding_texts(const sxml_function_call *call,
                                      const sxml_namespace_binding *binding,
                                      binding_texts *texts) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    *texts = (binding_texts){.prefix = {.bytes = NULL, .byte_count = 0},
                             .namespace_uri = {.bytes = NULL, .byte_count = 0}};
    sxml_status status =
        sxml_name_pool_get_namespace_prefix(name_pool, binding->namespace_prefix, &texts->prefix);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_name_pool_get_namespace_uri(name_pool, binding->namespace_uri, &texts->namespace_uri);
    }
    return status;
}

/* fn:in-scope-prefixes($element as element()) as xs:string* (F&O 4.0,
   section 10.2.7): every prefix of the in-scope namespaces, the
   zero-length string for a default namespace; xml included. */
static sxml_status call_in_scope_prefixes(const sxml_function_call *call,
                                          sxml_sequence *result) {
    element_namespaces namespaces;
    sxml_status status = read_in_scope_namespaces(call, 0, &namespaces);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < namespaces.bindings.namespace_binding_count;
         binding_index += 1) {
        binding_texts texts;
        status = read_binding_texts(call, &namespaces.bindings.namespace_bindings[binding_index],
                                    &texts);
        sxml_sequence prefix = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_make_string(call->item_heap, texts.prefix, &prefix);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(builder, prefix, call->error_record);
            sxml_sequence_release(prefix);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* The entries of fn:in-scope-namespaces while they are made: key and value
   items owned. */
typedef struct namespace_entries {
    sxml_item_heap *item_heap;
    sxml_map_entry *entries;
    size_t entry_count;
} namespace_entries;

static void release_namespace_entries(namespace_entries *entries,
                                      size_t allocated_entry_count) {
    for (size_t entry_index = 0; entry_index < entries->entry_count; entry_index += 1) {
        sxml_item_release(entries->entries[entry_index].key);
        sxml_sequence_release(entries->entries[entry_index].value);
    }
    sxml_item_heap_release_bytes(
        entries->item_heap,
        (sxml_heap_bytes){.memory = entries->entries,
                          .byte_count = allocated_entry_count * sizeof(sxml_map_entry)});
}

/* fn:in-scope-namespaces($element as element()) as map((xs:NCName |
   enum('')), xs:anyURI) (F&O 4.0, section 10.2.6): prefix (an xs:NCName,
   or "" for the default namespace) to namespace URI. The row's result type
   is the wider map(xs:string, xs:anyURI). */
static sxml_status call_in_scope_namespaces(const sxml_function_call *call,
                                            sxml_sequence *result) {
    element_namespaces namespaces;
    sxml_status status = read_in_scope_namespaces(call, 0, &namespaces);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t binding_count = namespaces.bindings.namespace_binding_count;
    namespace_entries entries = {.item_heap = call->item_heap, .entries = NULL, .entry_count = 0};
    if (binding_count > 0) {
        void *memory = NULL;
        status = binding_count > SIZE_MAX / sizeof(sxml_map_entry)
                     ? SXML_STATUS_ARITHMETIC_OVERFLOW
                     : sxml_item_heap_allocate_bytes(
                           call->item_heap,
                           (sxml_allocation_request){
                               .byte_count = binding_count * sizeof(sxml_map_entry),
                               .alignment = alignof(sxml_map_entry)},
                           &memory);
        entries.entries = memory;
    }
    for (size_t binding_index = 0; status == SXML_STATUS_OK && binding_index < binding_count;
         binding_index += 1) {
        binding_texts texts;
        status = read_binding_texts(call, &namespaces.bindings.namespace_bindings[binding_index],
                                    &texts);
        sxml_sequence key = sxml_sequence_empty();
        sxml_sequence value = sxml_sequence_empty();
        /* A prefix as xs:NCName, the default namespace as "" (xs:string). */
        if (status == SXML_STATUS_OK) {
            status = make_string_item(
                call->item_heap,
                (string_item_request){.type = texts.prefix.byte_count > 0 ? SXML_TYPE_XS_NCNAME
                                                                          : SXML_TYPE_XS_STRING,
                                      .text = texts.prefix},
                &key);
        }
        if (status == SXML_STATUS_OK) {
            status = make_string_item(
                call->item_heap,
                (string_item_request){.type = SXML_TYPE_XS_ANY_URI, .text = texts.namespace_uri},
                &value);
        }
        sxml_item key_item = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                key, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
                &key_item);
        }
        if (status == SXML_STATUS_OK) {
            sxml_item_retain(key_item);
            entries.entries[entries.entry_count] =
                (sxml_map_entry){.key = key_item, .value = value};
            entries.entry_count += 1;
            value = sxml_sequence_empty();
        }
        sxml_sequence_release(key);
        sxml_sequence_release(value);
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries.entries,
                                .map_entry_count = entries.entry_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                .error_record = call->error_record},
            &map);
    }
    release_namespace_entries(&entries, binding_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    return status;
}

/* fn:namespace-uri-for-prefix($value as (xs:NCName | enum(''))?, $element
   as element()) as xs:anyURI? (F&O 4.0, section 10.2.8; the row takes the
   wider xs:string?): the URI bound to the prefix
   among the element's in-scope namespaces; the empty sequence or "" ask
   for the default namespace; the empty sequence when nothing is bound. */
static sxml_status call_namespace_uri_for_prefix(const sxml_function_call *call,
                                                 sxml_sequence *result) {
    sxml_string prefix;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &prefix);
    element_namespaces namespaces;
    if (status == SXML_STATUS_OK) {
        status = read_in_scope_namespaces(call, 1, &namespaces);
    }
    *result = sxml_sequence_empty();
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < namespaces.bindings.namespace_binding_count;
         binding_index += 1) {
        binding_texts texts;
        status = read_binding_texts(call, &namespaces.bindings.namespace_bindings[binding_index],
                                    &texts);
        if (status == SXML_STATUS_OK && sxml_byte_view_equals(texts.prefix, prefix.utf8)) {
            return make_string_item(
                call->item_heap,
                (string_item_request){.type = SXML_TYPE_XS_ANY_URI, .text = texts.namespace_uri},
                result);
        }
    }
    return status;
}

/* ------------------------------------------------- QNames from strings */

static const sxml_static_error invalid_lexical_qname_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCA0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value is not a QName of the required form (F&O 4.0, fn:parse-QName, "
        "fn:resolve-QName)"),
};

static const sxml_static_error unbound_prefix_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FONS0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "No namespace is bound to the prefix (F&O 4.0, fn:resolve-QName)"),
};

/* Whether a text is an xs:NCName, tested by making the item. */
typedef struct ncname_check {
    bool is_ncname;
} ncname_check;

static sxml_status test_ncname(sxml_item_heap *item_heap, sxml_byte_view text,
                               ncname_check *check) {
    sxml_sequence item = sxml_sequence_empty();
    const sxml_status status = make_string_item(
        item_heap, (string_item_request){.type = SXML_TYPE_XS_NCNAME, .text = text}, &item);
    check->is_ncname = status == SXML_STATUS_OK;
    sxml_sequence_release(item);
    return status == SXML_STATUS_INVALID_ARGUMENT ? SXML_STATUS_OK : status;
}

/* A lexical QName split at its colon: prefix (empty for none) and local
   part, both checked to be NCNames (Namespaces in XML 1.0, section 4). */
typedef struct lexical_qname {
    bool is_valid;
    sxml_byte_view prefix;
    sxml_byte_view local_name;
} lexical_qname;

static sxml_status split_lexical_qname(sxml_item_heap *item_heap, sxml_byte_view text,
                                       lexical_qname *qname) {
    *qname = (lexical_qname){.is_valid = false,
                             .prefix = {.bytes = text.bytes, .byte_count = 0},
                             .local_name = text};
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] == ':') {
            qname->prefix.byte_count = byte_index;
            qname->local_name = (sxml_byte_view){.bytes = text.bytes + byte_index + 1,
                                                 .byte_count = text.byte_count - byte_index - 1};
            break;
        }
    }
    const bool has_colon = qname->local_name.bytes != text.bytes;
    ncname_check local_check = {.is_ncname = false};
    ncname_check prefix_check = {.is_ncname = true};
    sxml_status status = test_ncname(item_heap, qname->local_name, &local_check);
    if (status == SXML_STATUS_OK && has_colon) {
        status = test_ncname(item_heap, qname->prefix, &prefix_check);
    }
    qname->is_valid = local_check.is_ncname && prefix_check.is_ncname;
    return status;
}

/* The three texts of a QName to make. */
typedef struct qname_texts {
    sxml_byte_view namespace_uri;
    sxml_byte_view prefix;
    sxml_byte_view local_name;
} qname_texts;

/* Interns the texts and makes the xs:QName item, owned, in *result. */
static sxml_status make_qname(sxml_item_heap *item_heap, const qname_texts *texts,
                              sxml_sequence *result) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier expanded_name = {.index = 0};
    sxml_namespace_prefix_identifier namespace_prefix = {.index = 0};
    sxml_status status =
        sxml_name_pool_intern_namespace_uri(name_pool, texts->namespace_uri, &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = texts->local_name},
            &expanded_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(name_pool, texts->prefix, &namespace_prefix);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = expanded_name,
                                                      .namespace_prefix = namespace_prefix}}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

static bool is_xml_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static sxml_byte_view trim_whitespace(sxml_byte_view text) {
    while (text.byte_count > 0 && is_xml_whitespace(text.bytes[0])) {
        text.bytes += 1;
        text.byte_count -= 1;
    }
    while (text.byte_count > 0 && is_xml_whitespace(text.bytes[text.byte_count - 1])) {
        text.byte_count -= 1;
    }
    return text;
}

/* "prefix:local" resolved with the statically known namespaces of the
   running code, as xs:QName($value) does (F&O 4.0, section 19.1.9; D84):
   an unbound prefix is the cast's FONS0004. */
static sxml_status cast_to_qname(const sxml_function_call *call, sxml_byte_view text,
                                 sxml_sequence *result) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_function_library_make_string(call->item_heap, text, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_unit *unit = call->dynamic_context == NULL
                                   ? NULL
                                   : sxml_dynamic_context_get_running_unit(call->dynamic_context);
    status = sxml_cast_make_value(
        call->item_heap,
        &(sxml_cast_request){
            .value = value,
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .allows_empty = false,
            .namespace_bindings = unit == NULL
                                      ? (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                                                      .namespace_binding_count = 0}
                                      : sxml_ir_unit_get_static_namespaces(unit),
            .error_record = call->error_record},
        result);
    sxml_sequence_release(value);
    return status;
}

/*
 * fn:parse-QName($value as xs:string?) as xs:QName? (F&O 4.0, section
 * 10.1.2): the value without leading and trailing whitespace is an
 * NCName (no namespace), a lexical QName prefix:local (prefix from the
 * static context), or a URIQualifiedName Q{uri}local or Q{uri}prefix:local
 * (BracedURILiteral by the XPath rules: no "{" or "}" inside). Anything
 * else, and a prefix with a zero-length URI, is FOCA0002.
 */
static sxml_status call_parse_qname(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    sxml_string value;
    status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view text = trim_whitespace(value.utf8);
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view name = text;
    const bool is_uri_qualified =
        text.byte_count >= 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{';
    if (is_uri_qualified) {
        size_t close = 2;
        while (close < text.byte_count && text.bytes[close] != '}' && text.bytes[close] != '{') {
            close += 1;
        }
        if (close == text.byte_count || text.bytes[close] != '}') {
            return sxml_function_library_report(call, &invalid_lexical_qname_error);
        }
        namespace_uri = (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = close - 2};
        name = (sxml_byte_view){.bytes = text.bytes + close + 1,
                                .byte_count = text.byte_count - close - 1};
    }
    lexical_qname qname;
    status = split_lexical_qname(call->item_heap, name, &qname);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!qname.is_valid ||
        (is_uri_qualified && qname.prefix.byte_count > 0 && namespace_uri.byte_count == 0)) {
        return sxml_function_library_report(call, &invalid_lexical_qname_error);
    }
    if (!is_uri_qualified && qname.prefix.byte_count > 0) {
        return cast_to_qname(call, text, result);
    }
    return make_qname(call->item_heap,
                      &(qname_texts){.namespace_uri = namespace_uri,
                                     .prefix = qname.prefix,
                                     .local_name = qname.local_name},
                      result);
}

/*
 * fn:resolve-QName($value as xs:string?, $element as element()) as
 * xs:QName? (F&O 4.0, section 10.1.3): the prefix of the lexical QName
 * (or "" for none, the default namespace) looked up among the in-scope
 * namespaces of $element; no default namespace means no namespace. The
 * prefix is kept. FOCA0002 for a value that is not a lexical QName,
 * FONS0004 for a prefix the element does not bind.
 */
static sxml_status call_resolve_qname(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    sxml_string value;
    status = sxml_function_library_read_string_argument(call, 0, &value);
    lexical_qname qname;
    if (status == SXML_STATUS_OK) {
        status = split_lexical_qname(call->item_heap, value.utf8, &qname);
    }
    if (status == SXML_STATUS_OK && !qname.is_valid) {
        return sxml_function_library_report(call, &invalid_lexical_qname_error);
    }
    element_namespaces namespaces;
    if (status == SXML_STATUS_OK) {
        status = read_in_scope_namespaces(call, 1, &namespaces);
    }
    bool is_bound = false;
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    for (size_t binding_index = 0; status == SXML_STATUS_OK && !is_bound &&
                                   binding_index < namespaces.bindings.namespace_binding_count;
         binding_index += 1) {
        binding_texts texts;
        status = read_binding_texts(call, &namespaces.bindings.namespace_bindings[binding_index],
                                    &texts);
        if (status == SXML_STATUS_OK && sxml_byte_view_equals(texts.prefix, qname.prefix)) {
            is_bound = true;
            namespace_uri = texts.namespace_uri;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_bound && qname.prefix.byte_count > 0) {
        return sxml_function_library_report(call, &unbound_prefix_error);
    }
    return make_qname(call->item_heap,
                      &(qname_texts){.namespace_uri = namespace_uri,
                                     .prefix = qname.prefix,
                                     .local_name = qname.local_name},
                      result);
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter qname_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:QName?")};
static const sxml_function_parameter element_parameters[] = {
    SXML_REQUIRED_PARAMETER("element", "element()")};
static const sxml_function_parameter parse_qname_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?")};
static const sxml_function_parameter resolve_qname_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("element", "element()")};
static const sxml_function_parameter namespace_uri_for_prefix_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("element", "element()")};

static const sxml_native_function qname_functions[] = {
    SXML_FUNCTION_ROW("local-name-from-QName", qname_parameters, "xs:NCName?", 0,
                      call_local_name_from_qname),
    SXML_FUNCTION_ROW("prefix-from-QName", qname_parameters, "xs:NCName?", 0,
                      call_prefix_from_qname),
    SXML_FUNCTION_ROW("namespace-uri-from-QName", qname_parameters, "xs:anyURI?", 0,
                      call_namespace_uri_from_qname),
    SXML_FUNCTION_ROW("expanded-QName", qname_parameters, "xs:string?", 0, call_expanded_qname),
    SXML_FUNCTION_ROW("in-scope-prefixes", element_parameters, "xs:string*", 0,
                      call_in_scope_prefixes),
    SXML_FUNCTION_ROW("in-scope-namespaces", element_parameters, "map(xs:string, xs:anyURI)", 0,
                      call_in_scope_namespaces),
    SXML_FUNCTION_ROW("namespace-uri-for-prefix", namespace_uri_for_prefix_parameters,
                      "xs:anyURI?", 0, call_namespace_uri_for_prefix),
    /* Context-dependent: the static namespaces of the running code. */
    SXML_FUNCTION_ROW("parse-QName", parse_qname_parameters, "xs:QName?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_parse_qname),
    SXML_FUNCTION_ROW("resolve-QName", resolve_qname_parameters, "xs:QName?", 0,
                      call_resolve_qname),
};

const sxml_native_function_view sxml_function_library_qname = SXML_LIBRARY_PART(qname_functions);
