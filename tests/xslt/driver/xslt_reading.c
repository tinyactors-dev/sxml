/*
 * Reading the elements of the XSLT 4.0 test suite (xslt_reading.h).
 * Imitates the helpers of tests/qt4/driver/catalog.c and environment.c.
 */
#include "xslt_reading.h"

#include <stdalign.h>

#include "suite_path.h"
#include "sxml/name.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

sxml_status sxml_xslt_test_document_load(
    const sxml_qt4_document_reader *reader,
    const sxml_qt4_document_request *request, sxml_qt4_document *document) {
    sxml_qt4_document loaded = {.arena_tree = NULL,
                                .tree = NULL,
                                .name_pool = NULL,
                                .catalog_namespace = {.index = 0}};
    sxml_status status = sxml_qt4_document_load(reader, request, &loaded);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_name_pool_intern_namespace_uri(
        loaded.name_pool, SXML_TEXT(SXML_XSLT_TEST_CATALOG_NAMESPACE_URI),
        &loaded.catalog_namespace);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_document_unload(&loaded);
        return status;
    }
    *document = loaded;
    return SXML_STATUS_OK;
}

sxml_status
sxml_xslt_test_allocate_array(sxml_arena *arena,
                              sxml_xslt_test_array_request request,
                              void **memory) {
    if (request.element_count == 0) {
        *memory = NULL;
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    const sxml_status status = sxml_size_multiply(
        request.element_count, request.element_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_allocate(
        arena,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.alignment},
        memory);
}

sxml_status sxml_xslt_test_collect_children(
    const sxml_xslt_test_element_reader *reader,
    sxml_xslt_test_children_request request, sxml_xslt_test_node_list *list) {
    sxml_node_index *nodes = NULL;
    size_t node_count = 0;
    /* Two passes: count, then fill an array of that size. */
    for (size_t pass_index = 0; pass_index < 2; pass_index += 1) {
        sxml_qt4_element_cursor cursor = {.document = reader->document,
                                          .has_next_node = false,
                                          .next_node = {.index = 0}};
        sxml_status status = sxml_qt4_element_cursor_start(
            reader->document, request.parent, &cursor);
        size_t filled_count = 0;
        while (status == SXML_STATUS_OK) {
            sxml_qt4_element_step step = {
                .has_element = false,
                .element = {.index = 0},
                .local_name = sxml_xslt_test_empty_text};
            status = sxml_qt4_element_cursor_next(&cursor, &step);
            if (status != SXML_STATUS_OK || !step.has_element) {
                break;
            }
            if (sxml_byte_view_equals(step.local_name, request.local_name)) {
                if (nodes != NULL) {
                    nodes[filled_count] = step.element;
                }
                filled_count += 1;
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        node_count = filled_count;
        if (pass_index == 1 || filled_count == 0) {
            break;
        }
        void *memory = NULL;
        status = sxml_xslt_test_allocate_array(
            reader->arena,
            (sxml_xslt_test_array_request){
                .element_count = filled_count,
                .element_byte_count = sizeof(sxml_node_index),
                .alignment = alignof(sxml_node_index)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        nodes = memory;
    }
    *list = (sxml_xslt_test_node_list){.nodes = nodes, .node_count = node_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_test_find_child(
    const sxml_xslt_test_element_reader *reader,
    sxml_xslt_test_children_request request,
    sxml_xslt_test_child_lookup *lookup) {
    lookup->is_found = false;
    sxml_qt4_element_cursor cursor = {.document = reader->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    sxml_status status = sxml_qt4_element_cursor_start(
        reader->document, request.parent, &cursor);
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_xslt_test_empty_text};
        status = sxml_qt4_element_cursor_next(&cursor, &step);
        if (status != SXML_STATUS_OK || !step.has_element) {
            break;
        }
        if (sxml_byte_view_equals(step.local_name, request.local_name)) {
            lookup->element = step.element;
            lookup->is_found = true;
            break;
        }
    }
    return status;
}

sxml_status
sxml_xslt_test_read_attribute(const sxml_xslt_test_element_reader *reader,
                              sxml_qt4_attribute_lookup lookup,
                              sxml_byte_view *value) {
    sxml_qt4_attribute_value found = {.is_present = false,
                                      .value = sxml_xslt_test_empty_text};
    const sxml_status status =
        sxml_qt4_document_find_attribute(reader->document, lookup, &found);
    if (status == SXML_STATUS_OK) {
        *value = found.value;
    }
    return status;
}

sxml_status sxml_xslt_test_read_flag(const sxml_xslt_test_element_reader *reader,
                                     sxml_xslt_test_flag_lookup lookup) {
    sxml_byte_view text = sxml_xslt_test_empty_text;
    const sxml_status status = sxml_xslt_test_read_attribute(
        reader,
        (sxml_qt4_attribute_lookup){.element = lookup.element,
                                    .local_name = lookup.local_name},
        &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* yesNoType and xs:boolean (XSD 1.1 Part 2, section 3.3.2). */
    if (sxml_byte_view_equals(text, SXML_TEXT("yes")) ||
        sxml_byte_view_equals(text, SXML_TEXT("true")) ||
        sxml_byte_view_equals(text, SXML_TEXT("1"))) {
        *lookup.value = true;
    } else if (sxml_byte_view_equals(text, SXML_TEXT("no")) ||
               sxml_byte_view_equals(text, SXML_TEXT("false")) ||
               sxml_byte_view_equals(text, SXML_TEXT("0"))) {
        *lookup.value = false;
    } else {
        *lookup.value = lookup.default_value;
    }
    return SXML_STATUS_OK;
}

sxml_status
sxml_xslt_test_read_file_attribute(const sxml_xslt_test_element_reader *reader,
                                   sxml_node_index element,
                                   sxml_byte_view *file_path) {
    sxml_byte_view reference = sxml_xslt_test_empty_text;
    const sxml_status status = sxml_xslt_test_read_attribute(
        reader,
        (sxml_qt4_attribute_lookup){.element = element,
                                    .local_name = SXML_TEXT("file")},
        &reference);
    if (status != SXML_STATUS_OK || reference.byte_count == 0) {
        *file_path = sxml_xslt_test_empty_text;
        return status;
    }
    return sxml_qt4_suite_path_resolve(
        &(sxml_qt4_suite_path_request){.containing_file =
                                           reader->containing_file,
                                       .reference = reference,
                                       .arena = reader->arena},
        file_path);
}

sxml_xslt_test_name sxml_xslt_test_no_name(void) {
    return (sxml_xslt_test_name){.lexical_name = sxml_xslt_test_empty_text,
                                 .namespace_uri = sxml_xslt_test_empty_text,
                                 .local_name = sxml_xslt_test_empty_text};
}

void sxml_xslt_test_report_problem(const sxml_xslt_test_element_reader *reader,
                                   sxml_xslt_test_problem_text text) {
    if (reader->problem == NULL) {
        return;
    }
    *reader->problem = (sxml_qt4_problem){
        .description = text.description,
        .subject = text.subject,
        .location = {.source_name = reader->containing_file,
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
}

/* Q{uri}local (XPath 4.0, section 2.3.1 "Expanded QNames"): writes the
   parts and returns true, or returns false for another form. */
static bool sxml_xslt_test_split_eqname(sxml_byte_view text,
                                        sxml_xslt_test_name *name) {
    if (text.byte_count < 3 || text.bytes[0] != 'Q' || text.bytes[1] != '{') {
        return false;
    }
    for (size_t byte_index = 2; byte_index < text.byte_count;
         byte_index += 1) {
        if (text.bytes[byte_index] == '}') {
            name->namespace_uri = (sxml_byte_view){
                .bytes = text.bytes + 2, .byte_count = byte_index - 2};
            name->local_name =
                (sxml_byte_view){.bytes = text.bytes + byte_index + 1,
                                 .byte_count = text.byte_count - byte_index - 1};
            return name->local_name.byte_count != 0;
        }
    }
    return false;
}

sxml_status
sxml_xslt_test_read_name(const sxml_xslt_test_element_reader *reader,
                         sxml_qt4_attribute_lookup lookup,
                         sxml_xslt_test_name *name) {
    sxml_byte_view text = sxml_xslt_test_empty_text;
    sxml_status status = sxml_xslt_test_read_attribute(reader, lookup, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_test_name result = sxml_xslt_test_no_name();
    result.lexical_name = text;
    if (text.byte_count == 0 || text.bytes[0] == '#') {
        *name = result;
        return SXML_STATUS_OK;
    }
    if (text.bytes[0] == 'Q' && text.byte_count > 1 && text.bytes[1] == '{') {
        if (!sxml_xslt_test_split_eqname(text, &result)) {
            sxml_xslt_test_report_problem(
                reader,
                (sxml_xslt_test_problem_text){
                    .description = SXML_TEXT("malformed EQName"),
                    .subject = text});
            return SXML_STATUS_MALFORMED_INPUT;
        }
        *name = result;
        return SXML_STATUS_OK;
    }
    size_t colon_index = 0;
    while (colon_index < text.byte_count && text.bytes[colon_index] != ':') {
        colon_index += 1;
    }
    if (colon_index == text.byte_count) {
        result.local_name = text;
        *name = result;
        return SXML_STATUS_OK;
    }
    const sxml_byte_view prefix = {.bytes = text.bytes,
                                   .byte_count = colon_index};
    sxml_qt4_attribute_value bound = {.is_present = false,
                                      .value = sxml_xslt_test_empty_text};
    if (colon_index != 0) {
        status = sxml_qt4_document_resolve_prefix(
            reader->document,
            (sxml_qt4_prefix_lookup){.element = lookup.element,
                                     .namespace_prefix = prefix},
            &bound);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!bound.is_present || colon_index + 1 == text.byte_count) {
        sxml_xslt_test_report_problem(
            reader, (sxml_xslt_test_problem_text){
                        .description = SXML_TEXT("unbound namespace prefix"),
                        .subject = text});
        return SXML_STATUS_MALFORMED_INPUT;
    }
    result.namespace_uri = bound.value;
    result.local_name =
        (sxml_byte_view){.bytes = text.bytes + colon_index + 1,
                         .byte_count = text.byte_count - colon_index - 1};
    *name = result;
    return SXML_STATUS_OK;
}
