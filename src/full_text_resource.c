/*
 * Stop word lists and thesauri of Full Text by URI (src/full_text_internal.h,
 * "Stop word lists and thesauri"; Full Text 3.0, 3.4.3 and 3.4.7;
 * packet 7.12c, D214; glossary `thesaurus_expansion`). Part of the
 * optional feature full-text.
 *
 * The lowering reads them while the query compiles, through the
 * processor's resource resolver, as SXML_RESOURCE_KIND_FULL_TEXT_STOP_WORDS
 * and SXML_RESOURCE_KIND_FULL_TEXT_THESAURUS (include/sxml/processor.h):
 * they are the statically known stop word lists and thesauri, and what
 * they hold becomes constants of the match program. A stop word list is
 * UTF-8 text, its words separated by whitespace. A thesaurus is an XML
 * document in the format of the W3C Full Text test suite: entry elements
 * with a term and synonyms, each synonym a term, a relationship and
 * synonyms of its own one level further. Its relations are read in one
 * pass over its elements in document order (each synonym relates its
 * parent's term to its own), so no walk recurses (D71).
 *
 * They are feature data (sxml_optional_feature_read_data, D189): the
 * shared path asks the resolver, and the readers here take the bytes or
 * the document node.
 *
 * Golden file: src/cldr.c (feature data read through
 * sxml_optional_feature_read_data).
 */
#include "full_text_internal.h"

#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ----------------------------------------------------------- Word lists */

static sxml_status add_word(sxml_full_text_word_list *list, sxml_byte_view word) {
    if (list->word_count == list->word_capacity) {
        const size_t capacity = list->word_capacity == 0 ? 32U : list->word_capacity * 2U;
        sxml_full_text_word *words = NULL;
        const sxml_status status = sxml_allocator_allocate(
            list->bytes.allocator,
            (sxml_allocation_request){.byte_count = capacity * sizeof(sxml_full_text_word),
                                      .alignment = alignof(sxml_full_text_word)},
            (void **)&words);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (list->word_count != 0) {
            memcpy(words, list->words, list->word_count * sizeof(sxml_full_text_word));
        }
        sxml_allocator_release(list->bytes.allocator, list->words);
        list->words = words;
        list->word_capacity = capacity;
    }
    const size_t start = list->bytes.byte_count;
    const sxml_status status = sxml_xml_buffer_append(&list->bytes, word);
    if (status == SXML_STATUS_OK && list->bytes.byte_count > UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (status == SXML_STATUS_OK) {
        list->words[list->word_count] =
            (sxml_full_text_word){.byte_offset = (uint32_t)start,
                                  .byte_count = (uint32_t)(list->bytes.byte_count - start)};
        list->word_count += 1;
    }
    return status;
}

sxml_byte_view sxml_full_text_word_at(const sxml_full_text_word_list *list, size_t word_index) {
    const sxml_full_text_word word = list->words[word_index];
    return (sxml_byte_view){.bytes = list->bytes.bytes + word.byte_offset,
                            .byte_count = word.byte_count};
}

void sxml_full_text_word_list_release(sxml_full_text_word_list *list) {
    sxml_xml_buffer_release(&list->bytes);
    sxml_allocator_release(list->bytes.allocator, list->words);
    list->words = NULL;
    list->word_count = 0;
    list->word_capacity = 0;
}

static bool is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* The words of a stop word list: runs of non-whitespace, a leading byte
   order mark dropped. */
static sxml_status read_stop_words(sxml_byte_view content, sxml_full_text_word_list *list) {
    size_t byte_offset = 0;
    if (content.byte_count >= 3 && content.bytes[0] == 0xEF && content.bytes[1] == 0xBB &&
        content.bytes[2] == 0xBF) {
        byte_offset = 3;
    }
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && byte_offset < content.byte_count) {
        if (is_space(content.bytes[byte_offset])) {
            byte_offset += 1;
            continue;
        }
        const size_t start = byte_offset;
        while (byte_offset < content.byte_count && !is_space(content.bytes[byte_offset])) {
            byte_offset += 1;
        }
        status = add_word(list, (sxml_byte_view){.bytes = content.bytes + start,
                                                  .byte_count = byte_offset - start});
    }
    return status;
}

/* ------------------------------------------------------------ Thesauri */

/* One thesaurus document being read. */
typedef struct thesaurus_reader {
    sxml_tree *tree;
    const sxml_name_pool *name_pool;
    sxml_full_text_word_list *list;
    sxml_xml_buffer text;
} thesaurus_reader;

static sxml_status describe(const thesaurus_reader *reader, uint32_t node_index,
                            sxml_node_description *description) {
    return sxml_node_describe((sxml_node_reference){.tree = reader->tree,
                                                    .node_index = {.index = node_index},
                                                    .namespace_number = 0},
                              description);
}

/* Whether description is an element of local name name (the thesaurus
   namespace is not checked: the format is sxml's choice, 3.4.3). */
typedef struct name_check {
    const sxml_node_description *description;
    const char *name;
} name_check;

static bool is_element(const thesaurus_reader *reader, name_check check) {
    if (check.description->node_kind != SXML_NODE_KIND_ELEMENT) {
        return false;
    }
    sxml_name_parts parts;
    if (sxml_name_pool_get_name_parts(reader->name_pool, check.description->node_name, &parts) !=
        SXML_STATUS_OK) {
        return false;
    }
    return sxml_byte_view_equals(parts.local_name,
                                 (sxml_byte_view){.bytes = (const uint8_t *)check.name,
                                                  .byte_count = strlen(check.name)});
}

/* The string value of the child element of element named name into
   reader->text (empty when it has none). */
typedef struct child_text {
    uint32_t element;
    const char *name;
} child_text;

static sxml_status read_child_text(thesaurus_reader *reader, child_text request) {
    reader->text.byte_count = 0;
    sxml_node_description description;
    sxml_status status = describe(reader, request.element, &description);
    if (status != SXML_STATUS_OK || !description.has_children) {
        return status;
    }
    for (uint32_t child = description.first_child.index; status == SXML_STATUS_OK;) {
        sxml_node_description child_description;
        status = describe(reader, child, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (is_element(reader, (name_check){.description = &child_description,
                                            .name = request.name})) {
            for (uint32_t node_index = child + 1;
                 status == SXML_STATUS_OK && node_index <= child_description.last_descendant.index;
                 node_index += 1) {
                sxml_node_description text_description;
                status = describe(reader, node_index, &text_description);
                sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
                if (status == SXML_STATUS_OK && text_description.node_kind == SXML_NODE_KIND_TEXT) {
                    status = sxml_node_get_content(
                        (sxml_node_reference){.tree = reader->tree,
                                              .node_index = {.index = node_index},
                                              .namespace_number = 0},
                        &content);
                    if (status == SXML_STATUS_OK) {
                        status = sxml_xml_buffer_append(&reader->text, content);
                    }
                }
            }
            return status;
        }
        if (!child_description.has_next_sibling) {
            break;
        }
        child = child_description.next_sibling.index;
    }
    return status;
}

static sxml_byte_view reader_text(const thesaurus_reader *reader) {
    return (sxml_byte_view){.bytes = reader->text.bytes, .byte_count = reader->text.byte_count};
}

/* Every synonym element, in document order: (its parent's term, its
   relationship, its term). */
static sxml_status read_thesaurus(thesaurus_reader *reader, sxml_node_reference root) {
    sxml_node_description root_description;
    sxml_status status = sxml_node_describe(root, &root_description);
    for (uint32_t node_index = root.node_index.index + 1;
         status == SXML_STATUS_OK && node_index <= root_description.last_descendant.index;
         node_index += 1) {
        sxml_node_description description;
        status = describe(reader, node_index, &description);
        if (status != SXML_STATUS_OK ||
            !is_element(reader, (name_check){.description = &description, .name = "synonym"}) ||
            !description.has_parent) {
            continue;
        }
        status = read_child_text(reader,
                                 (child_text){.element = description.parent.index, .name = "term"});
        if (status == SXML_STATUS_OK) {
            status = add_word(reader->list, reader_text(reader));
        }
        if (status == SXML_STATUS_OK) {
            status = read_child_text(reader, (child_text){.element = node_index, .name = "relationship"});
        }
        if (status == SXML_STATUS_OK) {
            status = add_word(reader->list, reader_text(reader));
        }
        if (status == SXML_STATUS_OK) {
            status = read_child_text(reader, (child_text){.element = node_index, .name = "term"});
        }
        if (status == SXML_STATUS_OK) {
            status = add_word(reader->list, reader_text(reader));
        }
    }
    return status;
}

/* ---------------------------------------------------------- Reading */

/* What the feature data reader of one resource reads into. */
typedef struct resource_reading {
    sxml_processor *processor;
    sxml_full_text_word_list *list;
    /* The URI as written, the source name of a thesaurus parsed from
       bytes. */
    sxml_byte_view uri_reference;
    /* False for a thesaurus that is not well-formed XML. */
    bool is_usable;
} resource_reading;

static sxml_status read_stop_word_bytes(void *state, sxml_byte_view content) {
    resource_reading *reading = state;
    return read_stop_words(content, reading->list);
}

static sxml_status read_thesaurus_document(void *state, sxml_item document) {
    resource_reading *reading = state;
    sxml_node_reference root;
    sxml_status status = sxml_item_get_node(document, &root);
    if (status == SXML_STATUS_OK) {
        thesaurus_reader reader = {.tree = root.tree,
                                   .name_pool = sxml_processor_get_name_pool(reading->processor),
                                   .list = reading->list,
                                   .text = {.allocator = reading->list->bytes.allocator}};
        status = read_thesaurus(&reader, root);
        sxml_xml_buffer_release(&reader.text);
    }
    return status;
}

static sxml_status read_thesaurus_bytes(void *state, sxml_byte_view content) {
    resource_reading *reading = state;
    sxml_item document = {0};
    sxml_status status = sxml_processor_parse_document(
        reading->processor,
        &(sxml_xml_parse_request){.input = content,
                                  .source_name = reading->uri_reference,
                                  .document_uri = {.bytes = NULL, .byte_count = 0},
                                  .base_uri = {.bytes = NULL, .byte_count = 0},
                                  .encoding_name = {.bytes = NULL, .byte_count = 0}},
        &document);
    if (status != SXML_STATUS_OK) {
        /* Not a well-formed thesaurus: it cannot be used (FTST0018). */
        const bool is_malformed = status == SXML_STATUS_MALFORMED_INPUT ||
                                  status == SXML_STATUS_UNEXPECTED_END_OF_INPUT ||
                                  status == SXML_STATUS_INVALID_UTF8 ||
                                  status == SXML_STATUS_UNSUPPORTED_FEATURE;
        reading->is_usable = !is_malformed;
        return is_malformed ? SXML_STATUS_OK : status;
    }
    status = read_thesaurus_document(state, document);
    sxml_item_release(document);
    return status;
}

/* A stop word list is BYTES only; a thesaurus BYTES or DOCUMENT_NODE
   (include/sxml/processor.h). */
static const sxml_feature_data_reader_operations stop_word_reader_operations = {
    .read = read_stop_word_bytes,
    .read_document = NULL,
};

static const sxml_feature_data_reader_operations thesaurus_reader_operations = {
    .read = read_thesaurus_bytes,
    .read_document = read_thesaurus_document,
};

sxml_status sxml_full_text_read_resource(const sxml_full_text_resource_request *request,
                                         sxml_full_text_word_list *list,
                                         sxml_full_text_resource_outcome *outcome) {
    resource_reading reading = {.processor = request->processor,
                                .list = list,
                                .uri_reference = request->uri_reference,
                                .is_usable = true};
    sxml_feature_data_outcome data_outcome = {.is_available = false};
    /* 3.4.3, 3.4.7: a URI literal, resolved against the static base URI
       (XQuery 4.0, 4.5.2 for URI literals); the default list or thesaurus
       is asked for with no URI. */
    const sxml_status status = sxml_optional_feature_read_data(
        &(sxml_feature_data_request){
            .processor = request->processor,
            .resource_kind = request->resource_kind,
            .uri = request->uri_reference,
            .base_uri = request->base_uri,
            .reader = {.state = &reading,
                       .operations = request->resource_kind ==
                                             SXML_RESOURCE_KIND_FULL_TEXT_STOP_WORDS
                                         ? &stop_word_reader_operations
                                         : &thesaurus_reader_operations}},
        &data_outcome);
    outcome->is_found = data_outcome.is_available && reading.is_usable;
    return status;
}
