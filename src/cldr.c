/*
 * CLDR data (src/cldr_internal.h; glossary `cldr_file`,
 * `distinguished_path`, `locale_chain`; packet 7.17, D221): language
 * tags, locale chains, CLDR file URIs, LDML files read into tables of
 * distinguished paths, and lookups that follow inheritance and aliases
 * (UTS #35, Part 1, sections 3, 4.1 and 5).
 *
 * Golden file: src/collation_tailoring.c of packet 7.15b (the language
 * tag reader, the locale names and the LDML tree builder moved here from
 * it, so that collation and formatting read CLDR by one mechanism),
 * src/timezone.c (feature data named by a path under a URN).
 *
 * No recursion (D71): the tree builder keeps a fixed stack of element
 * depths; lookups and path rewriting are loops over fixed buffers.
 */
#include "cldr_internal.h"

#include "memory_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "sort_internal.h"

#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/xml.h"

#include <stdalign.h>
#include <string.h>

/* The deepest element kept; deeper ones are skipped (LDML has about ten
   levels). */
#define SXML_CLDR_MAXIMUM_DEPTH 32U
/* The most aliases one lookup follows. */
#define SXML_CLDR_MAXIMUM_ALIAS_COUNT 16U

static uint8_t lower_ascii(uint8_t byte) {
    return byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + 0x20U) : byte;
}

static uint8_t upper_ascii(uint8_t byte) {
    return byte >= 'a' && byte <= 'z' ? (uint8_t)(byte - 0x20U) : byte;
}

static bool view_is(sxml_byte_view view, const char *text) {
    return sxml_byte_view_equals(view, sxml_byte_view_from_c_string(text));
}

static bool starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 || memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

/* -------------------------------------------------------- Language tags */

bool sxml_cldr_lower_tag(sxml_byte_view text, sxml_cldr_tag_text *lowered) {
    lowered->byte_count = 0;
    if (text.byte_count > sizeof(lowered->bytes)) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        lowered->bytes[byte_index] = lower_ascii(text.bytes[byte_index]);
    }
    lowered->byte_count = text.byte_count;
    return true;
}

static bool is_tag_separator(uint8_t byte) { return byte == '-' || byte == '_'; }

void sxml_cldr_read_language_tag(sxml_byte_view lower_case_text, sxml_cldr_language_tag *tag) {
    memset(tag, 0, sizeof(*tag));
    tag->is_valid = true;
    const sxml_byte_view text = lower_case_text;
    size_t position = 0;
    bool is_in_unicode_extension = false;
    bool is_in_other_extension = false;
    bool is_after_collation_key = false;
    while (position <= text.byte_count) {
        size_t end = position;
        while (end < text.byte_count && !is_tag_separator(text.bytes[end])) {
            end += 1;
        }
        const sxml_byte_view subtag = {.bytes = text.bytes + position, .byte_count = end - position};
        position = end + 1;
        if (subtag.byte_count == 0 || subtag.byte_count > 8) {
            tag->is_valid = false;
            return;
        }
        if (is_after_collation_key) {
            tag->collation_type = subtag;
            is_after_collation_key = false;
            continue;
        }
        if (is_in_other_extension) {
            continue;
        }
        if (is_in_unicode_extension) {
            /* The "co" keyword (UTS #35, Part 1, section 3.6); a key is
               two characters, its values longer. */
            if (view_is(subtag, "co") && tag->collation_type.byte_count == 0) {
                is_after_collation_key = true;
                continue;
            }
            tag->has_other_extension = true;
            continue;
        }
        if (subtag.byte_count == 1) {
            if (tag->subtag_count == 0) {
                tag->is_valid = false;
                return;
            }
            if (subtag.bytes[0] == 'u') {
                is_in_unicode_extension = true;
            } else {
                is_in_other_extension = true;
                tag->has_other_extension = true;
            }
            continue;
        }
        if (tag->subtag_count == SXML_CLDR_MAXIMUM_SUBTAG_COUNT) {
            tag->is_valid = false;
            return;
        }
        tag->subtags[tag->subtag_count] = subtag;
        tag->subtag_count += 1;
    }
    if (is_after_collation_key || tag->subtag_count == 0) {
        tag->is_valid = false;
    }
}

/* Writes the file name of the first subtag_count subtags of tag. */
typedef struct locale_truncation {
    const sxml_cldr_language_tag *tag;
    size_t subtag_count;
} locale_truncation;

static bool make_locale_name(locale_truncation truncation, sxml_cldr_locale_name *name) {
    name->byte_count = 0;
    for (size_t subtag_index = 0; subtag_index < truncation.subtag_count; subtag_index += 1) {
        const sxml_byte_view subtag = truncation.tag->subtags[subtag_index];
        if (name->byte_count + subtag.byte_count + 1 > sizeof(name->bytes)) {
            return false;
        }
        if (subtag_index > 0) {
            name->bytes[name->byte_count] = '_';
            name->byte_count += 1;
        }
        const bool is_script = subtag_index > 0 && subtag.byte_count == 4 &&
                               !(subtag.bytes[0] >= '0' && subtag.bytes[0] <= '9');
        for (size_t byte_index = 0; byte_index < subtag.byte_count; byte_index += 1) {
            const uint8_t byte = subtag.bytes[byte_index];
            /* Only letters and digits reach a file name. */
            if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9'))) {
                return false;
            }
            uint8_t written = byte;
            if (subtag_index > 0) {
                written = is_script && byte_index > 0 ? byte : upper_ascii(byte);
            }
            name->bytes[name->byte_count] = written;
            name->byte_count += 1;
        }
    }
    return true;
}

static void set_root_name(sxml_cldr_locale_name *name) {
    static const uint8_t root_name[] = {'r', 'o', 'o', 't'};
    memcpy(name->bytes, root_name, sizeof(root_name));
    name->byte_count = sizeof(root_name);
}

bool sxml_cldr_locale_is_root(const sxml_cldr_locale_name *name) {
    return view_is((sxml_byte_view){.bytes = name->bytes, .byte_count = name->byte_count}, "root");
}

bool sxml_cldr_make_locale_chain(const sxml_cldr_language_tag *tag,
                                 sxml_cldr_locale_chain *chain) {
    chain->name_count = 0;
    if (!tag->is_valid) {
        return false;
    }
    const bool is_root_tag = tag->subtag_count == 1 && (view_is(tag->subtags[0], "und") ||
                                                        view_is(tag->subtags[0], "root"));
    if (!is_root_tag) {
        for (size_t level = tag->subtag_count; level > 0; level -= 1) {
            if (!make_locale_name((locale_truncation){.tag = tag, .subtag_count = level},
                                  &chain->names[chain->name_count])) {
                chain->name_count = 0;
                return false;
            }
            chain->name_count += 1;
        }
    }
    set_root_name(&chain->names[chain->name_count]);
    chain->name_count += 1;
    return true;
}

/* --------------------------------------------------------------- URIs */

bool sxml_cldr_make_uri(const sxml_cldr_uri_parts *parts, sxml_cldr_uri *uri) {
    const sxml_byte_view pieces[4] = {parts->uri_prefix, parts->directory, parts->file_name,
                                      SXML_BYTE_VIEW_FROM_LITERAL(".xml")};
    uri->byte_count = 0;
    for (size_t piece_index = 0; piece_index < 4; piece_index += 1) {
        const sxml_byte_view piece = pieces[piece_index];
        if (uri->byte_count + piece.byte_count > sizeof(uri->bytes)) {
            uri->byte_count = 0;
            return false;
        }
        if (piece.byte_count > 0) {
            memcpy(uri->bytes + uri->byte_count, piece.bytes, piece.byte_count);
        }
        uri->byte_count += piece.byte_count;
    }
    return true;
}

/* ----------------------------------------------------------- Byte text */

/* Bytes in allocator memory that grow by doubling. */
typedef struct byte_text {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} byte_text;

static sxml_status append_text(byte_text *text, sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t needed = 0;
    sxml_status status = sxml_size_add(text->byte_count, bytes.byte_count, &needed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (needed > text->byte_capacity) {
        size_t capacity = text->byte_capacity < 256 ? 256 : text->byte_capacity;
        while (status == SXML_STATUS_OK && capacity < needed) {
            status = sxml_size_multiply(capacity, 2, &capacity);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                text->allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (text->byte_count > 0) {
            memcpy(memory, text->bytes, text->byte_count);
        }
        sxml_allocator_release(text->allocator, text->bytes);
        text->bytes = memory;
        text->byte_capacity = capacity;
    }
    memcpy(text->bytes + text->byte_count, bytes.bytes, bytes.byte_count);
    text->byte_count = needed;
    return SXML_STATUS_OK;
}

static void release_text(byte_text *text) {
    sxml_allocator_release(text->allocator, text->bytes);
    text->bytes = NULL;
    text->byte_count = 0;
    text->byte_capacity = 0;
}

static sxml_byte_view text_view(const byte_text *text) {
    return (sxml_byte_view){.bytes = text->bytes, .byte_count = text->byte_count};
}

/* ------------------------------------------------------- Reading files */

/* An entry while the file is read: offsets into the text, which may
   move. */
typedef struct read_entry {
    size_t path_offset;
    size_t path_byte_count;
    size_t value_offset;
    size_t value_byte_count;
    bool is_alias;
} read_entry;

/* Where the reader of one file is. depth 1 is the root element. */
typedef struct file_reader {
    const sxml_name_pool *name_pool;
    sxml_cldr_path_prefix_view kept_paths;
    size_t depth;
    /* The depth of the element whose subtree is skipped; 0 when none. */
    size_t skipped_depth;
    /* The path of the element at each depth, as a length of path. */
    size_t path_lengths[SXML_CLDR_MAXIMUM_DEPTH + 2U];
    bool has_child[SXML_CLDR_MAXIMUM_DEPTH + 2U];
    bool is_alias[SXML_CLDR_MAXIMUM_DEPTH + 2U];
    /* The element just started, before its attributes are settled. */
    bool is_start_pending;
    sxml_byte_view pending_name;
    byte_text pending_attributes;
    byte_text alias_target;
    byte_text path;
    byte_text value;
    /* Paths and values of the entries. */
    byte_text entry_text;
    read_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
} file_reader;

static sxml_status local_name_of(const file_reader *reader, sxml_name_identifier name,
                                 sxml_byte_view *local_name) {
    sxml_name_parts parts;
    const sxml_status status = sxml_name_pool_get_name_parts(reader->name_pool, name, &parts);
    if (status == SXML_STATUS_OK) {
        *local_name = parts.local_name;
    }
    return status;
}

/* True when the path is one of the kept prefixes, below one, or above
   one (a segment boundary follows). */
static bool is_kept_path(const file_reader *reader, sxml_byte_view path) {
    if (reader->kept_paths.prefix_count == 0) {
        return true;
    }
    for (size_t prefix_index = 0; prefix_index < reader->kept_paths.prefix_count;
         prefix_index += 1) {
        const sxml_byte_view prefix = reader->kept_paths.prefixes[prefix_index];
        if (starts_with(path, prefix) &&
            (path.byte_count == prefix.byte_count || path.bytes[prefix.byte_count] == '/')) {
            return true;
        }
        if (starts_with(prefix, path) &&
            (path.byte_count == 0 || prefix.bytes[path.byte_count] == '/')) {
            return true;
        }
    }
    return false;
}

static sxml_status add_entry(file_reader *reader, sxml_byte_view path, sxml_byte_view value) {
    if (reader->entry_count == reader->entry_capacity) {
        size_t capacity = reader->entry_capacity < 64 ? 64 : reader->entry_capacity * 2U;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate_array(
            reader->entry_text.allocator,
            (sxml_array_allocation_request){.element_count = capacity,
                                            .element_size = sizeof(read_entry),
                                            .element_alignment = alignof(read_entry)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (reader->entry_count > 0) {
            memcpy(memory, reader->entries, reader->entry_count * sizeof(read_entry));
        }
        sxml_allocator_release(reader->entry_text.allocator, reader->entries);
        reader->entries = memory;
        reader->entry_capacity = capacity;
    }
    read_entry *entry = &reader->entries[reader->entry_count];
    entry->path_offset = reader->entry_text.byte_count;
    entry->path_byte_count = path.byte_count;
    sxml_status status = append_text(&reader->entry_text, path);
    entry->value_offset = reader->entry_text.byte_count;
    entry->value_byte_count = value.byte_count;
    if (status == SXML_STATUS_OK) {
        status = append_text(&reader->entry_text, value);
    }
    entry->is_alias = reader->is_alias[reader->depth];
    if (status == SXML_STATUS_OK) {
        reader->entry_count += 1;
    }
    return status;
}

/* Gives the element just started its segment of the path, or skips it. */
static sxml_status settle_start(file_reader *reader) {
    if (!reader->is_start_pending) {
        return SXML_STATUS_OK;
    }
    reader->is_start_pending = false;
    const size_t depth = reader->depth;
    reader->path_lengths[depth] = reader->path.byte_count;
    reader->has_child[depth] = false;
    reader->is_alias[depth] = view_is(reader->pending_name, "alias");
    reader->value.byte_count = 0;
    if (depth == 1 || reader->is_alias[depth]) {
        /* The root element has no segment; an alias stands for its
           parent. */
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    if (reader->path.byte_count > 0) {
        status = append_text(&reader->path, SXML_BYTE_VIEW_FROM_LITERAL("/"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_text(&reader->path, reader->pending_name);
    }
    if (status == SXML_STATUS_OK) {
        status = append_text(&reader->path, text_view(&reader->pending_attributes));
    }
    if (status == SXML_STATUS_OK && !is_kept_path(reader, text_view(&reader->path))) {
        reader->skipped_depth = depth;
    }
    return status;
}

static sxml_status read_start_document(void *state, const sxml_document_start_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status read_end_document(void *state) {
    (void)state;
    return SXML_STATUS_OK;
}

static sxml_status read_start_element(void *state, const sxml_element_start_event *event) {
    file_reader *reader = state;
    sxml_status status = settle_start(reader);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (reader->depth <= SXML_CLDR_MAXIMUM_DEPTH) {
        reader->has_child[reader->depth] = true;
    }
    reader->depth += 1;
    if (reader->skipped_depth != 0) {
        return SXML_STATUS_OK;
    }
    if (reader->depth > SXML_CLDR_MAXIMUM_DEPTH) {
        /* Too deep: skipped with its subtree, the path unchanged. */
        reader->skipped_depth = reader->depth;
        reader->path_lengths[reader->depth] = reader->path.byte_count;
        return SXML_STATUS_OK;
    }
    status = local_name_of(reader, event->element_name, &reader->pending_name);
    reader->is_start_pending = status == SXML_STATUS_OK;
    reader->pending_attributes.byte_count = 0;
    reader->alias_target.byte_count = 0;
    return status;
}

static sxml_status read_add_namespace_binding(void *state,
                                              const sxml_namespace_binding *namespace_binding) {
    (void)state;
    (void)namespace_binding;
    return SXML_STATUS_OK;
}

static sxml_status read_add_attribute(void *state, const sxml_attribute_event *event) {
    file_reader *reader = state;
    if (!reader->is_start_pending) {
        return SXML_STATUS_OK;
    }
    sxml_byte_view local_name = {.bytes = NULL, .byte_count = 0};
    sxml_status status = local_name_of(reader, event->attribute_name, &local_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (view_is(reader->pending_name, "alias")) {
        /* Only aliases within a locale (source="locale") exist in CLDR. */
        return view_is(local_name, "path") ? append_text(&reader->alias_target, event->attribute_value)
                                           : SXML_STATUS_OK;
    }
    /* Not distinguishing (UTS #35, Part 1, section 5.1.1). */
    if (view_is(local_name, "draft") || view_is(local_name, "references")) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view pieces[5] = {SXML_BYTE_VIEW_FROM_LITERAL("[@"), local_name,
                                      SXML_BYTE_VIEW_FROM_LITERAL("='"), event->attribute_value,
                                      SXML_BYTE_VIEW_FROM_LITERAL("']")};
    for (size_t piece_index = 0; status == SXML_STATUS_OK && piece_index < 5; piece_index += 1) {
        status = append_text(&reader->pending_attributes, pieces[piece_index]);
    }
    return status;
}

static sxml_status read_end_element(void *state) {
    file_reader *reader = state;
    sxml_status status = settle_start(reader);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t depth = reader->depth;
    if (reader->skipped_depth != 0) {
        if (depth == reader->skipped_depth) {
            reader->skipped_depth = 0;
            reader->path.byte_count = reader->path_lengths[depth];
        }
        reader->depth -= 1;
        return SXML_STATUS_OK;
    }
    if (depth > 1 && !reader->has_child[depth]) {
        status = add_entry(reader, text_view(&reader->path),
                           reader->is_alias[depth] ? text_view(&reader->alias_target)
                                                   : text_view(&reader->value));
    }
    reader->path.byte_count = reader->path_lengths[depth];
    reader->depth -= 1;
    return status;
}

static sxml_status read_add_text(void *state, const sxml_text_event *event) {
    file_reader *reader = state;
    const sxml_status status = settle_start(reader);
    if (status != SXML_STATUS_OK || reader->skipped_depth != 0 || reader->depth < 2) {
        return status;
    }
    return append_text(&reader->value, event->text);
}

static sxml_status read_add_comment(void *state, const sxml_comment_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status read_add_processing_instruction(
    void *state, const sxml_processing_instruction_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status read_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    (void)state;
    (void)entity;
    return SXML_STATUS_OK;
}

static const sxml_tree_builder_operations file_builder_operations = {
    .start_document = read_start_document,
    .end_document = read_end_document,
    .start_element = read_start_element,
    .add_namespace_binding = read_add_namespace_binding,
    .add_attribute = read_add_attribute,
    .end_element = read_end_element,
    .add_text = read_add_text,
    .add_comment = read_add_comment,
    .add_processing_instruction = read_add_processing_instruction,
    .add_unparsed_entity = read_add_unparsed_entity,
};

/* Reading one file as feature data. */
typedef struct file_reading {
    sxml_processor *processor;
    file_reader *reader;
    /* The document parsed; false for data that is not well-formed. */
    bool is_read;
} file_reading;

static sxml_status read_file_data(void *state, sxml_byte_view content) {
    file_reading *reading = state;
    sxml_processor *processor = reading->processor;
    sxml_xml_parser *parser = NULL;
    sxml_status status = sxml_xml_parser_create(
        processor->allocator,
        &(sxml_xml_parser_configuration){.allocator = processor->allocator,
                                         .name_pool = processor->name_pool,
                                         .limits = processor->limits.xml_limits},
        &parser);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_parse_request request;
    memset(&request, 0, sizeof(request));
    request.input = content;
    status = sxml_xml_parser_parse(
        parser, &request,
        (sxml_tree_builder){.state = reading->reader, .operations = &file_builder_operations});
    sxml_xml_parser_destroy(parser);
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE || status == SXML_STATUS_LIMIT_EXCEEDED) {
        /* Not CLDR data: the file counts as absent. */
        return SXML_STATUS_OK;
    }
    reading->is_read = status == SXML_STATUS_OK;
    return status;
}

static const sxml_feature_data_reader_operations file_reader_operations = {
    .read = read_file_data,
};

static bool entry_is_before(const void *state, sxml_sort_pair pair) {
    const sxml_cldr_entry *entries = state;
    const size_t first_index = *(const size_t *)pair.first;
    const size_t second_index = *(const size_t *)pair.second;
    const sxml_byte_view first = entries[first_index].path;
    const sxml_byte_view second = entries[second_index].path;
    const size_t common = first.byte_count < second.byte_count ? first.byte_count
                                                                : second.byte_count;
    const int order = common == 0 ? 0 : memcmp(first.bytes, second.bytes, common);
    if (order != 0) {
        return order < 0;
    }
    if (first.byte_count != second.byte_count) {
        return first.byte_count < second.byte_count;
    }
    /* Document order among equal paths. */
    return first_index < second_index;
}

/* Turns the reader's entries into the file's. */
static sxml_status finish_file(file_reader *reader, sxml_cldr_file *file) {
    const size_t entry_count = reader->entry_count;
    void *entries = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        file->allocator,
        (sxml_array_allocation_request){.element_count = entry_count,
                                        .element_size = sizeof(sxml_cldr_entry),
                                        .element_alignment = alignof(sxml_cldr_entry)},
        &entries);
    void *indexes = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            file->allocator,
            (sxml_array_allocation_request){.element_count = entry_count,
                                            .element_size = sizeof(size_t),
                                            .element_alignment = alignof(size_t)},
            &indexes);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(file->allocator, entries);
        return status;
    }
    sxml_cldr_entry *made = entries;
    size_t *sorted_indexes = indexes;
    const uint8_t *text = reader->entry_text.bytes;
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        const read_entry *read = &reader->entries[entry_index];
        made[entry_index] = (sxml_cldr_entry){
            .path = {.bytes = text + read->path_offset, .byte_count = read->path_byte_count},
            .value = {.bytes = text + read->value_offset, .byte_count = read->value_byte_count},
            .is_alias = read->is_alias};
        sorted_indexes[entry_index] = entry_index;
    }
    sxml_sort_in_place(&(sxml_sort_request){.elements = sorted_indexes,
                                            .element_count = entry_count,
                                            .element_size = sizeof(size_t),
                                            .is_before = entry_is_before,
                                            .state = made});
    file->entries = made;
    file->entry_count = entry_count;
    file->sorted_indexes = sorted_indexes;
    file->text = reader->entry_text.bytes;
    reader->entry_text = (byte_text){.allocator = reader->entry_text.allocator};
    return SXML_STATUS_OK;
}

sxml_status sxml_cldr_file_read(const sxml_cldr_file_request *request, sxml_cldr_file *file) {
    sxml_processor *processor = request->processor;
    const sxml_allocator allocator = file->allocator;
    memset(file, 0, sizeof(*file));
    file->allocator = allocator;
    file_reader reader;
    memset(&reader, 0, sizeof(reader));
    reader.name_pool = processor->name_pool;
    reader.kept_paths = request->kept_paths;
    reader.pending_attributes.allocator = allocator;
    reader.alias_target.allocator = allocator;
    reader.path.allocator = allocator;
    reader.value.allocator = allocator;
    reader.entry_text.allocator = allocator;
    file_reading reading = {.processor = processor, .reader = &reader, .is_read = false};
    sxml_feature_data_outcome outcome = {.is_available = false};
    sxml_status status = sxml_optional_feature_read_data(
        &(sxml_feature_data_request){
            .processor = processor,
            .resource_kind = request->resource_kind,
            .uri = request->uri,
            .reader = {.state = &reading, .operations = &file_reader_operations}},
        &outcome);
    if (status == SXML_STATUS_OK && reading.is_read && reader.entry_count > 0) {
        status = finish_file(&reader, file);
    }
    if (status == SXML_STATUS_OK) {
        file->is_served = reading.is_read;
    }
    release_text(&reader.pending_attributes);
    release_text(&reader.alias_target);
    release_text(&reader.path);
    release_text(&reader.value);
    release_text(&reader.entry_text);
    sxml_allocator_release(allocator, reader.entries);
    return status;
}

void sxml_cldr_file_release(sxml_cldr_file *file) {
    sxml_allocator_release(file->allocator, file->entries);
    sxml_allocator_release(file->allocator, file->sorted_indexes);
    sxml_allocator_release(file->allocator, file->text);
    const sxml_allocator allocator = file->allocator;
    memset(file, 0, sizeof(*file));
    file->allocator = allocator;
}

/* The first entry of file with path whose alias flag is is_alias, by a
   binary search of the sorted indexes. */
typedef struct entry_query {
    sxml_byte_view path;
    bool is_alias;
} entry_query;

static int compare_paths(sxml_byte_view first, sxml_byte_view second) {
    const size_t common = first.byte_count < second.byte_count ? first.byte_count
                                                                : second.byte_count;
    const int order = common == 0 ? 0 : memcmp(first.bytes, second.bytes, common);
    if (order != 0) {
        return order;
    }
    if (first.byte_count == second.byte_count) {
        return 0;
    }
    return first.byte_count < second.byte_count ? -1 : 1;
}

static const sxml_cldr_entry *find_entry(const sxml_cldr_file *file, entry_query query) {
    size_t low = 0;
    size_t high = file->entry_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (compare_paths(file->entries[file->sorted_indexes[middle]].path, query.path) < 0) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    for (size_t position = low; position < file->entry_count; position += 1) {
        const sxml_cldr_entry *entry = &file->entries[file->sorted_indexes[position]];
        if (!sxml_byte_view_equals(entry->path, query.path)) {
            break;
        }
        if (entry->is_alias == query.is_alias) {
            return entry;
        }
    }
    return NULL;
}

const sxml_cldr_entry *sxml_cldr_file_find(const sxml_cldr_file *file, sxml_byte_view path) {
    return find_entry(file, (entry_query){.path = path, .is_alias = false});
}

/* ------------------------------------------------------- Path segments */

/* The offset of the last "/" between segments of path (outside quotes),
   or path.byte_count when there is none. */
static size_t last_separator(sxml_byte_view path) {
    size_t found = path.byte_count;
    bool is_quoted = false;
    for (size_t byte_index = 0; byte_index < path.byte_count; byte_index += 1) {
        const uint8_t byte = path.bytes[byte_index];
        if (byte == '\'') {
            is_quoted = !is_quoted;
        } else if (byte == '/' && !is_quoted) {
            found = byte_index;
        }
    }
    return found;
}

sxml_byte_view sxml_cldr_parent_path(sxml_byte_view path) {
    const size_t separator = last_separator(path);
    return (sxml_byte_view){.bytes = path.bytes,
                            .byte_count = separator == path.byte_count ? 0 : separator};
}

bool sxml_cldr_find_attribute(const sxml_cldr_attribute_query *query,
                              sxml_byte_view *attribute_value) {
    const sxml_byte_view path = query->path;
    const size_t separator = last_separator(path);
    size_t position = separator == path.byte_count ? 0 : separator + 1U;
    while (position < path.byte_count) {
        if (path.bytes[position] != '[' || position + 2 >= path.byte_count ||
            path.bytes[position + 1] != '@') {
            position += 1;
            continue;
        }
        const size_t name_start = position + 2;
        size_t name_end = name_start;
        while (name_end < path.byte_count && path.bytes[name_end] != '=') {
            name_end += 1;
        }
        if (name_end + 1 >= path.byte_count || path.bytes[name_end + 1] != '\'') {
            return false;
        }
        const size_t value_start = name_end + 2;
        size_t value_end = value_start;
        while (value_end < path.byte_count && path.bytes[value_end] != '\'') {
            value_end += 1;
        }
        const sxml_byte_view name = {.bytes = path.bytes + name_start,
                                     .byte_count = name_end - name_start};
        if (sxml_byte_view_equals(name, query->attribute_name)) {
            *attribute_value = (sxml_byte_view){.bytes = path.bytes + value_start,
                                                .byte_count = value_end - value_start};
            return true;
        }
        position = value_end + 1;
    }
    return false;
}

/* ------------------------------------------------------------- Lookups */

/* A path being rewritten. */
typedef struct path_text {
    uint8_t bytes[SXML_CLDR_MAXIMUM_PATH_BYTE_COUNT];
    size_t byte_count;
} path_text;

static sxml_byte_view path_view(const path_text *path) {
    return (sxml_byte_view){.bytes = path->bytes, .byte_count = path->byte_count};
}

static bool set_path(path_text *path, sxml_byte_view text) {
    if (text.byte_count > sizeof(path->bytes)) {
        return false;
    }
    if (text.byte_count > 0) {
        memmove(path->bytes, text.bytes, text.byte_count);
    }
    path->byte_count = text.byte_count;
    return true;
}

static bool append_path(path_text *path, sxml_byte_view text) {
    if (path->byte_count + text.byte_count > sizeof(path->bytes)) {
        return false;
    }
    if (text.byte_count > 0) {
        memcpy(path->bytes + path->byte_count, text.bytes, text.byte_count);
    }
    path->byte_count += text.byte_count;
    return true;
}

/* CLDR's inheritance marker: "inherit this value from the parent". */
static bool is_inheritance_marker(sxml_byte_view value) {
    return view_is(value, "\xE2\x86\x91\xE2\x86\x91\xE2\x86\x91");
}

/* The alias target joined to its base, the path of the element that has
   the alias: each "../" removes the base's last segment; "//ldml/" makes
   the target absolute. */
typedef struct alias_rewrite {
    sxml_byte_view base;
    sxml_byte_view target;
    sxml_byte_view rest;
} alias_rewrite;

static bool rewrite_path(alias_rewrite rewrite, path_text *result) {
    sxml_byte_view base = rewrite.base;
    sxml_byte_view target = rewrite.target;
    const sxml_byte_view absolute = SXML_BYTE_VIEW_FROM_LITERAL("//ldml/");
    if (starts_with(target, absolute)) {
        base.byte_count = 0;
        target.bytes += absolute.byte_count;
        target.byte_count -= absolute.byte_count;
    }
    const sxml_byte_view parent_step = SXML_BYTE_VIEW_FROM_LITERAL("../");
    while (starts_with(target, parent_step)) {
        base = sxml_cldr_parent_path(base);
        target.bytes += parent_step.byte_count;
        target.byte_count -= parent_step.byte_count;
    }
    path_text joined = {.byte_count = 0};
    const bool is_joined =
        append_path(&joined, base) &&
        (base.byte_count == 0 || target.byte_count == 0 ||
         append_path(&joined, SXML_BYTE_VIEW_FROM_LITERAL("/"))) &&
        append_path(&joined, target) && append_path(&joined, rewrite.rest);
    return is_joined && set_path(result, path_view(&joined));
}

/* Replaces path's longest prefix that has an alias in the chain; false
   when none has one. */
static bool follow_alias(sxml_cldr_file_chain chain, path_text *path) {
    sxml_byte_view prefix = path_view(path);
    while (prefix.byte_count > 0) {
        for (size_t file_index = 0; file_index < chain.file_count; file_index += 1) {
            const sxml_cldr_entry *alias = find_entry(
                &chain.files[file_index], (entry_query){.path = prefix, .is_alias = true});
            if (alias != NULL) {
                path_text rest = {.byte_count = 0};
                if (!set_path(&rest, (sxml_byte_view){.bytes = path->bytes + prefix.byte_count,
                                                       .byte_count =
                                                           path->byte_count - prefix.byte_count})) {
                    return false;
                }
                return rewrite_path((alias_rewrite){.base = prefix,
                                                    .target = alias->value,
                                                    .rest = path_view(&rest)},
                                    path);
            }
        }
        prefix = sxml_cldr_parent_path(prefix);
    }
    return false;
}

bool sxml_cldr_find_value(const sxml_cldr_lookup *lookup, sxml_byte_view *value) {
    path_text path = {.byte_count = 0};
    if (!set_path(&path, lookup->path)) {
        return false;
    }
    for (size_t alias_count = 0; alias_count <= SXML_CLDR_MAXIMUM_ALIAS_COUNT; alias_count += 1) {
        for (size_t file_index = 0; file_index < lookup->chain.file_count; file_index += 1) {
            const sxml_cldr_entry *entry =
                sxml_cldr_file_find(&lookup->chain.files[file_index], path_view(&path));
            if (entry != NULL && !is_inheritance_marker(entry->value)) {
                *value = entry->value;
                return true;
            }
        }
        if (!follow_alias(lookup->chain, &path)) {
            return false;
        }
    }
    return false;
}
