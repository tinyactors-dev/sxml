/*
 * Elements to maps and back (F&O 4.0, section 14.6 "Converting elements
 * to maps"): fn:element-to-map-plan, fn:element-to-map and
 * fn:map-to-element, one library part (D101, D130).
 *
 * - fn:element-to-map walks the node indices of the element's subtree
 *   twice, never recursing (D71): in document order it chooses each
 *   element's layout (an element is converted only when its parent's
 *   layout converts its children), then in reverse document order it
 *   builds each element's value from its children's, which are done by
 *   then because descendants have the higher indices. The work record of
 *   a node is its element slot.
 * - fn:element-to-map-plan walks the same ranges once, grouping elements
 *   and attributes by name.
 * - fn:map-to-element walks the map with an explicit stack of frames and
 *   sends tree builder events to a new arena tree (D40, D110); the xml
 *   layout goes through the document loader (fn:parse-xml) and is copied
 *   in.
 * Maps from strings or names to positions, which the three need for
 * grouping and duplicate keys, are XDM maps (D37): no hash table of its
 * own.
 *
 * Golden file: src/function_library_json.c (options, trees made in heap
 * memory), rows as src/function_library_math.c.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "evaluator_machine_internal.h"
#include "function_library_core_internal.h"
#include "ir_internal.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "sxml/tree.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <math.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- Static errors */

static const sxml_static_error element_map_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The options or the conversion plan do not have the form F&O 4.0, "
        "section 14.6.5 requires, or the input is not a document with one "
        "element or an element"),
};

static const sxml_static_error element_map_layout_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0008"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An element cannot be converted with its layout and the plan has no "
        "fallback, or its layout is error"),
};

static const sxml_static_error element_map_form_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0009"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The map does not have the form fn:element-to-map makes, or the "
        "element made from it would not be well-formed"),
};

static const sxml_static_error element_map_cast_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0010"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Content cannot be cast to the type the conversion plan prescribes "
        "and liberal is false"),
};

static const sxml_static_error element_map_character_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCH0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An escape sequence in a name stands for a codepoint that is not an "
        "XML character"),
};

/* ----------------------------------------------------------- Layouts */

/* The element layouts (F&O 4.0, section 14.6.1) and the plan's actions. */
typedef enum element_map_layout {
    SXML_ELEMENT_MAP_LAYOUT_NONE,
    SXML_ELEMENT_MAP_LAYOUT_EMPTY,
    SXML_ELEMENT_MAP_LAYOUT_EMPTY_PLUS,
    SXML_ELEMENT_MAP_LAYOUT_SIMPLE,
    SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS,
    SXML_ELEMENT_MAP_LAYOUT_LIST,
    SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS,
    SXML_ELEMENT_MAP_LAYOUT_RECORD,
    SXML_ELEMENT_MAP_LAYOUT_SEQUENCE,
    SXML_ELEMENT_MAP_LAYOUT_MIXED,
    SXML_ELEMENT_MAP_LAYOUT_XML,
    SXML_ELEMENT_MAP_LAYOUT_ERROR,
    SXML_ELEMENT_MAP_LAYOUT_DEEP_SKIP,
} element_map_layout;

/* Layout names, by layout (index 0 unused). */
static const char *const element_map_layout_names[] = {
    "",       "empty",    "empty-plus", "simple", "simple-plus", "list",  "list-plus",
    "record", "sequence", "mixed",      "xml",    "error",       "deep-skip",
};

enum { SXML_ELEMENT_MAP_LAYOUT_COUNT = 13 };

/* The content types of a plan (F&O 4.0, section 14.6.9). */
typedef enum element_map_content_type {
    /* string, or no type: the value stays as it is. */
    SXML_ELEMENT_MAP_CONTENT_TYPE_STRING,
    SXML_ELEMENT_MAP_CONTENT_TYPE_INTEGER,
    SXML_ELEMENT_MAP_CONTENT_TYPE_DECIMAL,
    SXML_ELEMENT_MAP_CONTENT_TYPE_DOUBLE,
    SXML_ELEMENT_MAP_CONTENT_TYPE_BOOLEAN,
    /* Attributes only: leave the attribute out. */
    SXML_ELEMENT_MAP_CONTENT_TYPE_SKIP,
} element_map_content_type;

static const char *const element_map_content_type_names[] = {
    "string", "integer", "decimal", "double", "boolean", "skip",
};

enum { SXML_ELEMENT_MAP_CONTENT_TYPE_COUNT = 6 };

/* Layouts whose children are converted to maps of their own. */
static bool element_map_layout_converts_children(element_map_layout layout) {
    return layout == SXML_ELEMENT_MAP_LAYOUT_LIST || layout == SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS ||
           layout == SXML_ELEMENT_MAP_LAYOUT_RECORD ||
           layout == SXML_ELEMENT_MAP_LAYOUT_SEQUENCE || layout == SXML_ELEMENT_MAP_LAYOUT_MIXED;
}

/* --------------------------------------------------------------- Plan */

/* One entry of a conversion plan (F&O 4.0, section 14.6.5). */
typedef struct element_map_plan_entry {
    bool is_attribute;
    bool is_fallback;
    bool has_child_name;
    /* The element or attribute name; unused for the fallback. */
    sxml_name_identifier name;
    element_map_layout layout;
    sxml_name_identifier child_name;
    element_map_content_type content_type;
} element_map_plan_entry;

/* The name formats (F&O 4.0, section 14.6.8). */
typedef enum element_map_name_format {
    SXML_ELEMENT_MAP_NAME_FORMAT_DEFAULT,
    SXML_ELEMENT_MAP_NAME_FORMAT_EQNAME,
    SXML_ELEMENT_MAP_NAME_FORMAT_LOCAL,
    SXML_ELEMENT_MAP_NAME_FORMAT_LEXICAL,
} element_map_name_format;

enum {
    SXML_ELEMENT_MAP_OPTION_PLAN,
    SXML_ELEMENT_MAP_OPTION_ATTRIBUTE_MARKER,
    SXML_ELEMENT_MAP_OPTION_CONTENT_KEY,
    SXML_ELEMENT_MAP_OPTION_NAME_FORMAT,
    SXML_ELEMENT_MAP_OPTION_ESCAPE_NAMES,
    SXML_ELEMENT_MAP_OPTION_LIBERAL,
    SXML_ELEMENT_MAP_OPTION_COUNT,
};

/* The options of fn:element-to-map and fn:map-to-element (F&O 4.0,
   section 14.6.14), in the order of the enumeration above, read by the
   option parameter conventions (src/function_library_options.c). */
static const sxml_option_definition element_map_option_definitions[] = {
    SXML_FUNCTION_OPTION("plan", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("attribute-marker", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("content-key", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("name-format", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("escape-names", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("liberal", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
};

static const char *const element_map_name_format_names[] = {
    "default", "eqname", "local", "lexical",
};

/* What one call works with: the call, its options and its plan. */
typedef struct element_map_context {
    const sxml_function_call *call;
    sxml_item_heap *item_heap;
    sxml_name_pool *name_pool;
    /* Work memory: the dynamic context's, or the heap's. */
    sxml_allocator allocator;
    /* Borrowed from the options map. */
    sxml_byte_view attribute_marker;
    sxml_byte_view content_key;
    element_map_name_format name_format;
    bool escapes_names;
    bool is_liberal;
    /* The plan entries, owned (allocator), and an XDM map from a name key
       (see element_map_plan_key) to the entry position, owned. */
    element_map_plan_entry *plan_entries;
    size_t plan_entry_count;
    sxml_item plan_index;
    bool has_plan_index;
    /* The coerced option values, owned; the marker and the content key
       are borrowed from them. */
    sxml_option_values option_values;
    bool has_fallback;
    element_map_plan_entry fallback;
} element_map_context;

static void element_map_context_release(element_map_context *context) {
    sxml_function_library_release_options(&context->option_values);
    sxml_allocator_release(context->allocator, context->plan_entries);
    context->plan_entries = NULL;
    context->plan_entry_count = 0;
    if (context->has_plan_index) {
        sxml_item_release(context->plan_index);
        context->has_plan_index = false;
    }
}

static sxml_status element_map_report(const element_map_context *context,
                                      const sxml_static_error *error) {
    return sxml_error_record_report(context->call->error_record, error);
}

/* A yes-or-no outcome written by a function. */
typedef struct element_map_answer {
    bool is_true;
} element_map_answer;

/* An element or attribute name to look up. */
typedef struct element_map_name_query {
    sxml_name_identifier name;
    bool is_attribute;
} element_map_name_query;

/* The key of a name in the plan index: the name identifier, with bit 32
   set for attributes. */
static sxml_item element_map_plan_key(element_map_name_query query) {
    return sxml_atomic_item_from_integer((int64_t)query.name.index +
                                         (query.is_attribute ? (INT64_C(1) << 32) : 0));
}

/* Finds the plan entry of a name; NULL when the plan has none. */
static sxml_status element_map_find_plan_entry(const element_map_context *context,
                                               element_map_name_query query,
                                               const element_map_plan_entry **entry) {
    *entry = NULL;
    if (!context->has_plan_index) {
        return SXML_STATUS_OK;
    }
    sxml_map_lookup lookup;
    const sxml_status status = sxml_map_find(context->plan_index,
                                             element_map_plan_key((element_map_name_query){.name = query.name, .is_attribute = query.is_attribute}),
                                             &lookup);
    if (status == SXML_STATUS_OK && lookup.is_found) {
        sxml_atomic_datum datum;
        if (sxml_atomic_item_get_datum(lookup.entry.value.content, &datum) == SXML_STATUS_OK) {
            *entry = &context->plan_entries[datum.datum_details.integer];
        }
    }
    return status;
}

/* The text of a string-like atomic item (xs:string, xs:anyURI,
   xs:untypedAtomic); false for anything else. */
static bool element_map_item_text(sxml_item item, sxml_byte_view *text) {
    sxml_atomic_datum datum;
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_ATOMIC ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return false;
    }
    *text = datum.datum_details.string.utf8;
    return true;
}

/* True for a sequence of exactly one item, written to *item. */
static bool element_map_single_item(sxml_sequence sequence, sxml_item *item) {
    if (sxml_sequence_get_representation(sequence) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        uint64_t item_count = 0;
        if (sxml_sequence_count(sequence, NULL, &item_count) != SXML_STATUS_OK ||
            item_count != 1) {
            return false;
        }
        return sxml_sequence_get_item(sequence, (sxml_item_request){.item_index = 0,
                                                                    .error_record = NULL},
                                      item) == SXML_STATUS_OK;
    }
    *item = sequence.content;
    return true;
}

static bool element_map_is_empty(sxml_sequence sequence) {
    sxml_emptiness emptiness = {.is_empty = false};
    return sxml_sequence_test_emptiness(sequence, NULL, &emptiness) == SXML_STATUS_OK &&
           emptiness.is_empty;
}

/* ------------------------------------------------------------- Names */

/* A name of the form Q{uri}local split up; false for another form. */
typedef struct element_map_eqname {
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} element_map_eqname;

static bool element_map_split_eqname(sxml_byte_view text, element_map_eqname *eqname) {
    if (text.byte_count < 3 || text.bytes[0] != 'Q' || text.bytes[1] != '{') {
        return false;
    }
    for (size_t byte_index = 2; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] == '{') {
            return false;
        }
        if (text.bytes[byte_index] == '}') {
            eqname->namespace_uri =
                (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = byte_index - 2};
            eqname->local_name = (sxml_byte_view){.bytes = text.bytes + byte_index + 1,
                                                  .byte_count = text.byte_count - byte_index - 1};
            return true;
        }
    }
    return false;
}

/* A name to intern from its parts. */
typedef struct element_map_name_text {
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} element_map_name_text;

static sxml_status element_map_intern_name(const element_map_context *context,
                                           element_map_name_text name_text,
                                           sxml_name_identifier *name) {
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_status status = SXML_STATUS_OK;
    if (name_text.namespace_uri.byte_count > 0) {
        status = sxml_name_pool_intern_namespace_uri(context->name_pool, name_text.namespace_uri,
                                                     &namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            context->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = name_text.local_name},
            name);
    }
    return status;
}

/*
 * Reads a plan name: "local" or "Q{uri}local" (F&O 4.0, section 14.6.5).
 * *is_valid is false for any other form.
 */
typedef struct element_map_plan_name {
    bool is_valid;
    sxml_name_identifier name;
} element_map_plan_name;

static sxml_status element_map_read_plan_name(const element_map_context *context,
                                              sxml_byte_view text,
                                              element_map_plan_name *plan_name) {
    plan_name->is_valid = false;
    element_map_eqname eqname = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                                 .local_name = text};
    if (!element_map_split_eqname(text, &eqname) && !sxml_xml_text_is_ncname(text)) {
        return SXML_STATUS_OK;
    }
    if (!sxml_xml_text_is_ncname(eqname.local_name)) {
        return SXML_STATUS_OK;
    }
    plan_name->is_valid = true;
    return element_map_intern_name(
        context,
        (element_map_name_text){.namespace_uri = eqname.namespace_uri,
                                .local_name = eqname.local_name},
        &plan_name->name);
}

/* ------------------------------------------------------------ Options */

/* Finds text in a list of names; count when absent. */
typedef struct element_map_name_list {
    const char *const *names;
    size_t name_count;
} element_map_name_list;

static size_t element_map_find_name(element_map_name_list list, sxml_byte_view text) {
    for (size_t name_index = 0; name_index < list.name_count; name_index += 1) {
        if (sxml_byte_view_equals(text, sxml_byte_view_from_c_string(list.names[name_index]))) {
            return name_index;
        }
    }
    return list.name_count;
}

/* -------------------------------------------------------- Plan reading */

/* A field value of a plan record: absent, the empty sequence, or one
   string from a list of names (XPTY0004 otherwise). */
typedef struct element_map_plan_field {
    sxml_sequence value;
    element_map_name_list allowed_names;
} element_map_plan_field;

typedef struct element_map_field_reading {
    bool is_present;
    size_t name_index;
    sxml_byte_view text;
} element_map_field_reading;

static sxml_status element_map_read_plan_field(const element_map_context *context,
                                               element_map_plan_field field,
                                               element_map_field_reading *reading) {
    reading->is_present = false;
    if (element_map_is_empty(field.value)) {
        return SXML_STATUS_OK;
    }
    sxml_item item;
    if (!element_map_single_item(field.value, &item) ||
        !element_map_item_text(item, &reading->text)) {
        return element_map_report(context, &element_map_type_error);
    }
    reading->is_present = true;
    if (field.allowed_names.name_count > 0) {
        reading->name_index = element_map_find_name(field.allowed_names, reading->text);
        if (reading->name_index == field.allowed_names.name_count) {
            return element_map_report(context, &element_map_type_error);
        }
    }
    return SXML_STATUS_OK;
}

/* A plan record and where its entry goes. */
typedef struct element_map_plan_record {
    sxml_sequence value;
    bool is_attribute;
} element_map_plan_record;

/* Reads one plan record (F&O 4.0, sections 14.6.5.1 and 14.6.5.2) into
   *entry; every rule broken is XPTY0004. */
static sxml_status element_map_read_plan_record(const element_map_context *context,
                                                element_map_plan_record record,
                                                element_map_plan_entry *entry) {
    sxml_item map;
    if (!element_map_single_item(record.value, &map) ||
        sxml_item_get_kind(map) != SXML_ITEM_KIND_MAP) {
        return element_map_report(context, &element_map_type_error);
    }
    size_t field_count = 0;
    sxml_status status = sxml_map_get_size(map, &field_count);
    element_map_field_reading layout = {.is_present = false};
    element_map_field_reading child = {.is_present = false};
    element_map_field_reading content_type = {.is_present = false};
    for (size_t field_index = 0; status == SXML_STATUS_OK && field_index < field_count;
         field_index += 1) {
        sxml_map_entry field;
        status = sxml_map_get_entry(map, field_index, &field);
        sxml_byte_view field_name;
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!element_map_item_text(field.key, &field_name)) {
            return element_map_report(context, &element_map_type_error);
        }
        if (sxml_byte_view_equals(field_name, SXML_BYTE_VIEW_FROM_LITERAL("type"))) {
            status = element_map_read_plan_field(
                context,
                (element_map_plan_field){
                    .value = field.value,
                    .allowed_names = {element_map_content_type_names,
                                      record.is_attribute
                                          ? SXML_ELEMENT_MAP_CONTENT_TYPE_COUNT
                                          : SXML_ELEMENT_MAP_CONTENT_TYPE_COUNT - 1}},
                &content_type);
        } else if (!record.is_attribute &&
                   sxml_byte_view_equals(field_name, SXML_BYTE_VIEW_FROM_LITERAL("layout"))) {
            status = element_map_read_plan_field(
                context,
                (element_map_plan_field){
                    .value = field.value,
                    .allowed_names = {element_map_layout_names + 1,
                                      SXML_ELEMENT_MAP_LAYOUT_COUNT - 1}},
                &layout);
        } else if (!record.is_attribute &&
                   sxml_byte_view_equals(field_name, SXML_BYTE_VIEW_FROM_LITERAL("child"))) {
            status = element_map_read_plan_field(
                context,
                (element_map_plan_field){.value = field.value,
                                         .allowed_names = {NULL, 0}},
                &child);
        } else {
            return element_map_report(context, &element_map_type_error);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    entry->content_type = content_type.is_present
                              ? (element_map_content_type)content_type.name_index
                              : SXML_ELEMENT_MAP_CONTENT_TYPE_STRING;
    entry->layout = SXML_ELEMENT_MAP_LAYOUT_NONE;
    entry->has_child_name = false;
    if (record.is_attribute) {
        return SXML_STATUS_OK;
    }
    if (!layout.is_present) {
        return element_map_report(context, &element_map_type_error);
    }
    if (layout.name_index + 1 >= SXML_ELEMENT_MAP_LAYOUT_COUNT) {
        return element_map_report(context, &element_map_type_error);
    }
    entry->layout = (element_map_layout)(layout.name_index + 1);
    const bool is_list = entry->layout == SXML_ELEMENT_MAP_LAYOUT_LIST ||
                         entry->layout == SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS;
    const bool is_simple = entry->layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE ||
                           entry->layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS;
    if (is_list != child.is_present || (content_type.is_present && !is_simple)) {
        return element_map_report(context, &element_map_type_error);
    }
    if (child.is_present) {
        element_map_plan_name child_name = {.is_valid = false};
        status = element_map_read_plan_name(context, child.text, &child_name);
        if (status == SXML_STATUS_OK && !child_name.is_valid) {
            return element_map_report(context, &element_map_type_error);
        }
        entry->has_child_name = true;
        entry->child_name = child_name.name;
    }
    return status;
}

/* Reads the plan option into context: entries whose keys have none of
   the forms of F&O 4.0, section 14.6.5 are ignored, unchecked. */
static sxml_status element_map_read_plan(element_map_context *context, sxml_sequence plan) {
    if (element_map_is_empty(plan)) {
        return SXML_STATUS_OK;
    }
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(plan.content, &entry_count);
    if (status != SXML_STATUS_OK || entry_count == 0) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = entry_count * sizeof(element_map_plan_entry),
                                  .alignment = alignof(element_map_plan_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->plan_entries = memory;
    sxml_map_entry *index_entries = NULL;
    status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = entry_count * sizeof(sxml_map_entry),
                                  .alignment = alignof(sxml_map_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    index_entries = memory;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(plan.content, entry_index, &entry);
        sxml_byte_view key_text;
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!element_map_item_text(entry.key, &key_text)) {
            status = element_map_report(context, &element_map_type_error);
            break;
        }
        element_map_plan_entry plan_entry = {.is_attribute = false, .is_fallback = false};
        element_map_plan_name plan_name = {.is_valid = false};
        if (sxml_byte_view_equals(key_text, SXML_BYTE_VIEW_FROM_LITERAL("*"))) {
            plan_entry.is_fallback = true;
        } else {
            plan_entry.is_attribute = key_text.byte_count > 0 && key_text.bytes[0] == '@';
            const sxml_byte_view name_text =
                plan_entry.is_attribute
                    ? (sxml_byte_view){.bytes = key_text.bytes + 1,
                                       .byte_count = key_text.byte_count - 1}
                    : key_text;
            status = element_map_read_plan_name(context, name_text, &plan_name);
            if (status != SXML_STATUS_OK || !plan_name.is_valid) {
                continue;
            }
            plan_entry.name = plan_name.name;
        }
        status = element_map_read_plan_record(
            context,
            (element_map_plan_record){.value = entry.value,
                                      .is_attribute = plan_entry.is_attribute},
            &plan_entry);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (plan_entry.is_fallback) {
            context->fallback = plan_entry;
            context->has_fallback = true;
            continue;
        }
        /* Keys and values are inline integers: nothing to retain. */
        index_entries[context->plan_entry_count] = (sxml_map_entry){
            .key = element_map_plan_key((element_map_name_query){.name = plan_entry.name, .is_attribute = plan_entry.is_attribute}),
            .value = sxml_sequence_from_item(
                sxml_atomic_item_from_integer((int64_t)context->plan_entry_count))};
        context->plan_entries[context->plan_entry_count] = plan_entry;
        context->plan_entry_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            context->item_heap,
            &(sxml_map_request){.map_entries = {index_entries, context->plan_entry_count},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_LAST,
                                .error_record = NULL},
            &context->plan_index);
        context->has_plan_index = status == SXML_STATUS_OK;
    }
    sxml_allocator_release(context->allocator, index_entries);
    return status;
}

/* Reads the options argument (argument 1) into context. */
static sxml_status element_map_read_options(element_map_context *context) {
    sxml_option_values *values = &context->option_values;
    sxml_status status = sxml_function_library_read_options(
        context->call,
        &(sxml_option_request){
            .argument_index = 1,
            .definitions = {element_map_option_definitions,
                            sizeof(element_map_option_definitions) /
                                sizeof(element_map_option_definitions[0])}},
        values);
    context->attribute_marker = SXML_BYTE_VIEW_FROM_LITERAL("@");
    context->content_key = SXML_BYTE_VIEW_FROM_LITERAL("#content");
    context->name_format = SXML_ELEMENT_MAP_NAME_FORMAT_DEFAULT;
    if (status == SXML_STATUS_OK && values->is_present[SXML_ELEMENT_MAP_OPTION_NAME_FORMAT]) {
        sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
        (void)element_map_item_text(values->values[SXML_ELEMENT_MAP_OPTION_NAME_FORMAT].content,
                                    &text);
        const size_t format_index =
            element_map_find_name((element_map_name_list){element_map_name_format_names, 4}, text);
        if (format_index == 4) {
            status = element_map_report(context, &element_map_type_error);
        } else {
            context->name_format = (element_map_name_format)format_index;
        }
    }
    context->escapes_names = sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = values, .option_index = SXML_ELEMENT_MAP_OPTION_ESCAPE_NAMES});
    context->is_liberal = sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = values, .option_index = SXML_ELEMENT_MAP_OPTION_LIBERAL});
    if (status == SXML_STATUS_OK) {
        status = element_map_read_plan(context, values->values[SXML_ELEMENT_MAP_OPTION_PLAN]);
    }
    /* The marker and the content key are borrowed from the options map,
       which the call's argument keeps alive (the coerced string is the
       option's own item). */
    if (status == SXML_STATUS_OK) {
        sxml_byte_view text;
        if (values->is_present[SXML_ELEMENT_MAP_OPTION_ATTRIBUTE_MARKER] &&
            element_map_item_text(values->values[SXML_ELEMENT_MAP_OPTION_ATTRIBUTE_MARKER].content,
                                  &text)) {
            context->attribute_marker = text;
        }
        if (values->is_present[SXML_ELEMENT_MAP_OPTION_CONTENT_KEY] &&
            element_map_item_text(values->values[SXML_ELEMENT_MAP_OPTION_CONTENT_KEY].content,
                                  &text)) {
            context->content_key = text;
        }
    }
    return status;
}

/* ------------------------------------------------------------- Atoms */

/* An atomic item of a string type (xs:string, xs:untypedAtomic,
   xs:NCName) with the given UTF-8, owned. */
typedef struct element_map_text_item {
    sxml_builtin_type type;
    sxml_byte_view utf8;
} element_map_text_item;

static sxml_status element_map_make_text_item(const element_map_context *context,
                                              element_map_text_item request, sxml_item *item) {
    return sxml_atomic_item_make(
        context->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(request.type),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = request.utf8,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        item);
}

static bool element_map_is_whitespace(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (!sxml_xml_byte_is_whitespace(text.bytes[byte_index])) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------- Inferring a type */

/* What the strings seen so far say about their type (F&O 4.0, section
   14.6.3 "Inferring a data type"). Zero-initialize, then add strings. */
typedef struct element_map_type_evidence {
    bool has_strings;
    bool is_not_numeric;
    bool is_not_integer;
    bool is_not_decimal;
    bool is_not_boolean;
    bool has_leading_zero;
} element_map_type_evidence;

/* The shape of a number's lexical form: digits, fraction, exponent. */
typedef struct element_map_number_shape {
    bool is_number;
    bool has_point;
    bool has_exponent;
    bool has_leading_zero;
} element_map_number_shape;

/* Scans [+-]?(digits(.digits*)?|.digits)([eE][+-]?digits)? (the lexical
   space of xs:double without INF and NaN, XSD 1.1 Part 2, 3.3.5). */
static element_map_number_shape element_map_scan_number(sxml_byte_view text) {
    element_map_number_shape shape = {.is_number = false};
    size_t position = 0;
    if (position < text.byte_count && (text.bytes[position] == '+' || text.bytes[position] == '-')) {
        position += 1;
    }
    const size_t digits_start = position;
    size_t digit_count = 0;
    while (position < text.byte_count && text.bytes[position] >= '0' &&
           text.bytes[position] <= '9') {
        position += 1;
        digit_count += 1;
    }
    shape.has_leading_zero = digit_count >= 2 && text.bytes[digits_start] == '0';
    if (position < text.byte_count && text.bytes[position] == '.') {
        shape.has_point = true;
        position += 1;
        while (position < text.byte_count && text.bytes[position] >= '0' &&
               text.bytes[position] <= '9') {
            position += 1;
            digit_count += 1;
        }
    }
    if (digit_count == 0) {
        return shape;
    }
    if (position < text.byte_count && (text.bytes[position] == 'e' || text.bytes[position] == 'E')) {
        shape.has_exponent = true;
        position += 1;
        if (position < text.byte_count &&
            (text.bytes[position] == '+' || text.bytes[position] == '-')) {
            position += 1;
        }
        size_t exponent_digit_count = 0;
        while (position < text.byte_count && text.bytes[position] >= '0' &&
               text.bytes[position] <= '9') {
            position += 1;
            exponent_digit_count += 1;
        }
        if (exponent_digit_count == 0) {
            return shape;
        }
    }
    shape.is_number = position == text.byte_count;
    return shape;
}

static void element_map_add_evidence(element_map_type_evidence *evidence, sxml_byte_view text) {
    const sxml_byte_view trimmed = sxml_xml_text_trim_whitespace(text);
    const element_map_number_shape shape = element_map_scan_number(trimmed);
    evidence->has_strings = true;
    if (!shape.is_number) {
        evidence->is_not_numeric = true;
    }
    if (!shape.is_number || shape.has_point || shape.has_exponent) {
        evidence->is_not_integer = true;
    }
    if (!shape.is_number || shape.has_exponent) {
        evidence->is_not_decimal = true;
    }
    if (shape.is_number && !shape.has_point && !shape.has_exponent && shape.has_leading_zero) {
        evidence->has_leading_zero = true;
    }
    if (!sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("true")) &&
        !sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("false")) &&
        !sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("0")) &&
        !sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("1"))) {
        evidence->is_not_boolean = true;
    }
}

/* The rules of F&O 4.0, section 14.6.3, in order. */
static element_map_content_type element_map_infer_type(const element_map_type_evidence *evidence) {
    if (!evidence->has_strings) {
        return SXML_ELEMENT_MAP_CONTENT_TYPE_STRING;
    }
    if (!evidence->is_not_numeric) {
        if (!evidence->is_not_integer) {
            return evidence->has_leading_zero ? SXML_ELEMENT_MAP_CONTENT_TYPE_STRING
                                              : SXML_ELEMENT_MAP_CONTENT_TYPE_INTEGER;
        }
        return evidence->is_not_decimal ? SXML_ELEMENT_MAP_CONTENT_TYPE_DOUBLE
                                        : SXML_ELEMENT_MAP_CONTENT_TYPE_DECIMAL;
    }
    return evidence->is_not_boolean ? SXML_ELEMENT_MAP_CONTENT_TYPE_STRING
                                    : SXML_ELEMENT_MAP_CONTENT_TYPE_BOOLEAN;
}

/* Text to represent with a prescribed type (F&O 4.0, section 14.6.9). */
typedef struct element_map_content_request {
    sxml_byte_view text;
    element_map_content_type content_type;
} element_map_content_request;

/*
 * The value of untyped content: xs:untypedAtomic unless a type other than
 * string is prescribed and the text is not empty or whitespace; then the
 * text cast to that type, or, when it cannot be, FOJS0010 (liberal false)
 * or xs:untypedAtomic (liberal true).
 */
static sxml_status element_map_make_content(const element_map_context *context,
                                            element_map_content_request request,
                                            sxml_sequence *value) {
    static const sxml_builtin_type target_types[] = {
        SXML_TYPE_XS_UNTYPED_ATOMIC, SXML_TYPE_XS_INTEGER, SXML_TYPE_XS_DECIMAL,
        SXML_TYPE_XS_DOUBLE,         SXML_TYPE_XS_BOOLEAN, SXML_TYPE_XS_UNTYPED_ATOMIC,
    };
    if (request.content_type != SXML_ELEMENT_MAP_CONTENT_TYPE_STRING &&
        request.content_type != SXML_ELEMENT_MAP_CONTENT_TYPE_SKIP &&
        !element_map_is_whitespace(request.text)) {
        const sxml_status status = sxml_cast_make_value_from_text(
            context->item_heap,
            &(sxml_text_cast_request){
                .text = request.text,
                .target_type = sxml_builtin_type_identifier(target_types[request.content_type]),
                .namespace_bindings = {NULL, 0},
                .error_record = NULL},
            value);
        if (status != SXML_STATUS_DYNAMIC_ERROR && status != SXML_STATUS_TYPE_ERROR) {
            return status;
        }
        if (!context->is_liberal) {
            return element_map_report(context, &element_map_cast_error);
        }
    }
    sxml_item item = {.representation = 0};
    const sxml_status status = element_map_make_text_item(
        context,
        (element_map_text_item){.type = SXML_TYPE_XS_UNTYPED_ATOMIC, .utf8 = request.text},
        &item);
    if (status == SXML_STATUS_OK) {
        *value = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------ Escaped names */

static int element_map_hex_digit(uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    return -1;
}

/* Reads _xHHHH_ or _xHHHHHH_ at position; false when neither is there. */
typedef struct element_map_escape {
    uint32_t codepoint;
    size_t byte_count;
} element_map_escape;

static bool element_map_read_escape(sxml_byte_view text, size_t position,
                                    element_map_escape *escape) {
    if (position + 2 > text.byte_count || text.bytes[position] != '_' ||
        text.bytes[position + 1] != 'x') {
        return false;
    }
    static const size_t digit_counts[] = {4, 6};
    for (size_t form_index = 0; form_index < 2; form_index += 1) {
        const size_t digit_count = digit_counts[form_index];
        if (position + 2 + digit_count >= text.byte_count ||
            text.bytes[position + 2 + digit_count] != '_') {
            continue;
        }
        uint32_t codepoint = 0;
        bool is_hex = true;
        for (size_t digit_index = 0; digit_index < digit_count; digit_index += 1) {
            const int digit = element_map_hex_digit(text.bytes[position + 2 + digit_index]);
            is_hex = is_hex && digit >= 0;
            codepoint = codepoint * 16U + (uint32_t)(digit < 0 ? 0 : digit);
        }
        if (is_hex) {
            *escape = (element_map_escape){.codepoint = codepoint,
                                           .byte_count = digit_count + 3};
            return true;
        }
    }
    return false;
}

/* Appends a local name, decoding its escapes when the escape-names option
   is on (F&O 4.0, section 14.6.8): FOCH0001 for an escape of a codepoint
   that is not an XML character. */
static sxml_status element_map_append_decoded(const element_map_context *context,
                                              sxml_byte_view local_name,
                                              sxml_library_text *text) {
    if (!context->escapes_names) {
        return sxml_function_library_text_append(text, local_name);
    }
    if (sxml_byte_view_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("_x_"))) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    size_t piece_start = 0;
    size_t position = 0;
    while (status == SXML_STATUS_OK && position < local_name.byte_count) {
        element_map_escape escape;
        if (!element_map_read_escape(local_name, position, &escape)) {
            position += 1;
            continue;
        }
        if (!sxml_xml_codepoint_is_char(escape.codepoint)) {
            return element_map_report(context, &element_map_character_error);
        }
        status = sxml_function_library_text_append(
            text, (sxml_byte_view){.bytes = local_name.bytes + piece_start,
                                   .byte_count = position - piece_start});
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append_codepoint(text, escape.codepoint);
        }
        position += escape.byte_count;
        piece_start = position;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(
            text, (sxml_byte_view){.bytes = local_name.bytes + piece_start,
                                   .byte_count = local_name.byte_count - piece_start});
    }
    return status;
}

/* ------------------------------------------------------ Key formatting */

/* A name to write as a map key (F&O 4.0, section 14.6.8). */
typedef struct element_map_key_request {
    sxml_name_identifier name;
    sxml_namespace_prefix_identifier namespace_prefix;
    bool is_attribute;
    /* For elements: a top-level element, or the namespace of the parent. */
    bool is_top;
    sxml_namespace_uri_identifier parent_namespace;
    /* For attributes: the marker to put first. */
    sxml_byte_view attribute_marker;
} element_map_key_request;

static sxml_status element_map_append_key(const element_map_context *context,
                                          const element_map_key_request *request,
                                          sxml_library_text *text) {
    sxml_name_parts parts;
    sxml_status status = sxml_name_pool_get_name_parts(context->name_pool, request->name, &parts);
    if (status == SXML_STATUS_OK && request->is_attribute) {
        status = sxml_function_library_text_append(text, request->attribute_marker);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool has_namespace = parts.namespace_uri.index != SXML_NAMESPACE_NONE;
    bool writes_uri = false;
    bool writes_prefix = false;
    if (request->is_attribute && parts.namespace_uri.index == SXML_NAMESPACE_XML) {
        /* Attributes in the xml namespace are always xml:local. */
        status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("xml:"));
    } else {
        switch (context->name_format) {
        case SXML_ELEMENT_MAP_NAME_FORMAT_DEFAULT:
            writes_uri = request->is_attribute ? has_namespace
                         : request->is_top     ? has_namespace
                                               : parts.namespace_uri.index !=
                                                     request->parent_namespace.index;
            break;
        case SXML_ELEMENT_MAP_NAME_FORMAT_EQNAME:
            writes_uri = has_namespace;
            break;
        case SXML_ELEMENT_MAP_NAME_FORMAT_LOCAL:
            break;
        case SXML_ELEMENT_MAP_NAME_FORMAT_LEXICAL:
            writes_prefix = request->namespace_prefix.index != 0;
            break;
        }
    }
    if (status == SXML_STATUS_OK && writes_uri) {
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        if (has_namespace) {
            status = sxml_name_pool_get_namespace_uri(context->name_pool, parts.namespace_uri,
                                                      &namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("}"));
        }
    }
    if (status == SXML_STATUS_OK && writes_prefix) {
        sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
        status = sxml_name_pool_get_namespace_prefix(context->name_pool,
                                                     request->namespace_prefix, &prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, prefix);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL(":"));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_append_decoded(context, parts.local_name, text);
    }
    return status;
}

/* The key of request as an xs:string item, owned. */
static sxml_status element_map_make_key(const element_map_context *context,
                                        const element_map_key_request *request, sxml_item *key) {
    sxml_library_text text = {.item_heap = context->item_heap};
    sxml_status status = element_map_append_key(context, request, &text);
    if (status == SXML_STATUS_OK) {
        status = element_map_make_text_item(
            context,
            (element_map_text_item){.type = SXML_TYPE_XS_STRING,
                                    .utf8 = sxml_function_library_text_view(&text)},
            key);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* An xs:string item of a literal, owned. */
static sxml_status element_map_make_literal_key(const element_map_context *context,
                                                sxml_byte_view literal, sxml_item *key) {
    return element_map_make_text_item(
        context, (element_map_text_item){.type = SXML_TYPE_XS_STRING, .utf8 = literal}, key);
}

/* ------------------------------------------------------- Work arrays */

/* A growing array in the context's allocator. */
typedef struct element_map_buffer {
    void *elements;
    size_t element_count;
    size_t element_capacity;
} element_map_buffer;

/* Makes room for one more element of element_byte_count bytes. */
static sxml_status element_map_buffer_reserve(const element_map_context *context,
                                              element_map_buffer *buffer,
                                              size_t element_byte_count) {
    if (buffer->element_count < buffer->element_capacity) {
        return SXML_STATUS_OK;
    }
    const size_t capacity = buffer->element_capacity == 0 ? 8 : buffer->element_capacity * 2;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(capacity, element_byte_count, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(max_align_t)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (buffer->element_count > 0) {
        memcpy(memory, buffer->elements, buffer->element_count * element_byte_count);
    }
    sxml_allocator_release(context->allocator, buffer->elements);
    buffer->elements = memory;
    buffer->element_capacity = capacity;
    return SXML_STATUS_OK;
}

static void element_map_buffer_release(const element_map_context *context,
                                       element_map_buffer *buffer) {
    sxml_allocator_release(context->allocator, buffer->elements);
    *buffer = (element_map_buffer){.elements = NULL, .element_count = 0, .element_capacity = 0};
}

/* ------------------------------------------------------- Entry lists */

/* One entry being collected: its values combine into an array when a key
   comes again (F&O 4.0, section 14.6.8, last paragraphs). */
typedef struct element_map_pending_entry {
    sxml_item key;
    /* The values, owned, in order. */
    element_map_buffer values;
} element_map_pending_entry;

/* The entries of a map being made, and an XDM map from key to position. */
typedef struct element_map_entry_list {
    element_map_buffer entries;
    sxml_item index;
    bool has_index;
} element_map_entry_list;

static void element_map_entry_list_release(const element_map_context *context,
                                           element_map_entry_list *list) {
    element_map_pending_entry *entries = list->entries.elements;
    for (size_t entry_index = 0; entry_index < list->entries.element_count; entry_index += 1) {
        sxml_item_release(entries[entry_index].key);
        sxml_sequence *values = entries[entry_index].values.elements;
        for (size_t value_index = 0; value_index < entries[entry_index].values.element_count;
             value_index += 1) {
            sxml_sequence_release(values[value_index]);
        }
        element_map_buffer_release(context, &entries[entry_index].values);
    }
    element_map_buffer_release(context, &list->entries);
    if (list->has_index) {
        sxml_item_release(list->index);
        list->has_index = false;
    }
}

/* Finds key in the list: SIZE_MAX when absent. */
static sxml_status element_map_entry_list_find(const element_map_entry_list *list,
                                               sxml_item key, size_t *position) {
    *position = SIZE_MAX;
    if (!list->has_index) {
        return SXML_STATUS_OK;
    }
    sxml_map_lookup lookup;
    const sxml_status status = sxml_map_find(list->index, key, &lookup);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && lookup.is_found &&
        sxml_atomic_item_get_datum(lookup.entry.value.content, &datum) == SXML_STATUS_OK) {
        *position = (size_t)datum.datum_details.integer;
    }
    return status;
}

/* A key and a value to add, both owned: the list takes them. */
typedef struct element_map_entry_addition {
    sxml_item key;
    sxml_sequence value;
} element_map_entry_addition;

static sxml_status element_map_entry_list_add(const element_map_context *context,
                                              element_map_entry_list *list,
                                              element_map_entry_addition addition) {
    size_t position = SIZE_MAX;
    sxml_status status = SXML_STATUS_OK;
    if (!list->has_index) {
        status = sxml_map_make(context->item_heap,
                               &(sxml_map_request){.map_entries = {NULL, 0},
                                                   .duplicate_key_policy =
                                                       SXML_DUPLICATE_KEY_POLICY_USE_LAST,
                                                   .error_record = NULL},
                               &list->index);
        list->has_index = status == SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_entry_list_find(list, addition.key, &position);
    }
    if (status == SXML_STATUS_OK && position == SIZE_MAX) {
        status = element_map_buffer_reserve(context, &list->entries,
                                            sizeof(element_map_pending_entry));
        sxml_item updated = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_map_put(
                context->item_heap,
                &(sxml_map_update){.map = list->index,
                                   .key = addition.key,
                                   .value = sxml_sequence_from_item(sxml_atomic_item_from_integer(
                                       (int64_t)list->entries.element_count))},
                &updated);
        }
        if (status == SXML_STATUS_OK) {
            sxml_item_release(list->index);
            list->index = updated;
            position = list->entries.element_count;
            ((element_map_pending_entry *)list->entries.elements)[position] =
                (element_map_pending_entry){.key = addition.key,
                                            .values = {NULL, 0, 0}};
            list->entries.element_count += 1;
            addition.key = (sxml_item){.representation = 0};
        }
    }
    element_map_pending_entry *entry = NULL;
    if (status == SXML_STATUS_OK) {
        entry = &((element_map_pending_entry *)list->entries.elements)[position];
        status = element_map_buffer_reserve(context, &entry->values, sizeof(sxml_sequence));
    }
    if (status == SXML_STATUS_OK) {
        ((sxml_sequence *)entry->values.elements)[entry->values.element_count] = addition.value;
        entry->values.element_count += 1;
        addition.value = sxml_sequence_empty();
    }
    if (addition.key.representation != 0) {
        sxml_item_release(addition.key);
    }
    sxml_sequence_release(addition.value);
    return status;
}

/* A key text to look for in an entry list. */
typedef struct element_map_text_query {
    const element_map_entry_list *list;
    sxml_byte_view text;
} element_map_text_query;

/* True when the list has an entry with key text. */
static sxml_status element_map_entry_list_has_text(const element_map_context *context,
                                                   element_map_text_query query, element_map_answer *is_found) {
    const element_map_entry_list *list = query.list;
    const sxml_byte_view text = query.text;
    is_found->is_true = false;
    if (!list->has_index) {
        return SXML_STATUS_OK;
    }
    sxml_item key = {.representation = 0};
    sxml_status status = element_map_make_literal_key(context, text, &key);
    size_t position = SIZE_MAX;
    if (status == SXML_STATUS_OK) {
        status = element_map_entry_list_find(list, key, &position);
        sxml_item_release(key);
    }
    is_found->is_true = position != SIZE_MAX;
    return status;
}

/* The value of one entry: its value, or the array of its values. */
static sxml_status element_map_entry_value(const element_map_context *context,
                                           const element_map_pending_entry *entry,
                                           sxml_sequence *value) {
    const sxml_sequence *values = entry->values.elements;
    if (entry->values.element_count == 1) {
        sxml_sequence_retain(values[0]);
        *value = values[0];
        return SXML_STATUS_OK;
    }
    sxml_item array = {.representation = 0};
    const sxml_status status = sxml_array_make(
        context->item_heap,
        (sxml_sequence_view){.sequences = values, .sequence_count = entry->values.element_count},
        &array);
    if (status == SXML_STATUS_OK) {
        *value = sxml_sequence_from_item(array);
    }
    return status;
}

/* Makes the map of the list (in order of first appearance) and releases
   the list. */
static sxml_status element_map_entry_list_finish(const element_map_context *context,
                                                 element_map_entry_list *list,
                                                 sxml_sequence *map) {
    const size_t entry_count = list->entries.element_count;
    element_map_buffer made = {NULL, 0, 0};
    sxml_status status = SXML_STATUS_OK;
    const element_map_pending_entry *entries = list->entries.elements;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        status = element_map_buffer_reserve(context, &made, sizeof(sxml_map_entry));
        sxml_sequence value = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = element_map_entry_value(context, &entries[entry_index], &value);
        }
        if (status == SXML_STATUS_OK) {
            ((sxml_map_entry *)made.elements)[made.element_count] =
                (sxml_map_entry){.key = entries[entry_index].key, .value = value};
            made.element_count += 1;
        }
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            context->item_heap,
            &(sxml_map_request){.map_entries = {made.elements, made.element_count},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                                .error_record = NULL},
            &item);
    }
    for (size_t entry_index = 0; entry_index < made.element_count; entry_index += 1) {
        sxml_sequence_release(((sxml_map_entry *)made.elements)[entry_index].value);
    }
    element_map_buffer_release(context, &made);
    element_map_entry_list_release(context, list);
    if (status == SXML_STATUS_OK) {
        *map = sxml_sequence_from_item(item);
    }
    return status;
}

/* The single-entry map {key: value}; key and value owned, taken. */
static sxml_status element_map_make_single_entry_map(const element_map_context *context,
                                                     element_map_entry_addition addition,
                                                     sxml_sequence *map) {
    const sxml_map_entry entry = {.key = addition.key, .value = addition.value};
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_map_make(
        context->item_heap,
        &(sxml_map_request){.map_entries = {&entry, 1},
                            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                            .error_record = NULL},
        &item);
    sxml_item_release(addition.key);
    sxml_sequence_release(addition.value);
    if (status == SXML_STATUS_OK) {
        *map = sxml_sequence_from_item(item);
    }
    return status;
}

/* ---------------------------------------------- Element-to-map: shapes */

/* The work record of one node of the converted subtree (glossary
   element_slot). */
typedef struct element_slot {
    /* The value built for the element, owned while has_value. */
    sxml_sequence value;
    sxml_name_identifier child_name;
    element_map_layout layout;
    element_map_content_type content_type;
    bool is_converted;
    bool has_value;
    bool has_child_name;
} element_slot;

/* One call of fn:element-to-map at work. */
typedef struct element_map_conversion {
    element_map_context *context;
    sxml_node_reference root;
    element_slot *slots;
    size_t slot_count;
    /* Scratch: the child names of one element, to sort. */
    element_map_buffer child_names;
} element_map_conversion;

/* What the children and attributes of one element are like (the tests of
   F&O 4.0, sections 14.6.1 and 14.6.2 on a single element). */
typedef struct element_map_shape {
    size_t child_element_count;
    bool has_text;
    bool has_non_whitespace_text;
    bool has_attributes;
    bool has_one_child_name;
    bool has_distinct_child_names;
    sxml_name_identifier child_name;
} element_map_shape;

static sxml_namespace_uri_identifier element_map_namespace_of(const element_map_context *context,
                                                              sxml_name_identifier name) {
    sxml_name_parts parts;
    if (sxml_name_pool_get_name_parts(context->name_pool, name, &parts) != SXML_STATUS_OK) {
        return (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
    }
    return parts.namespace_uri;
}

/* True for an attribute in the xsi namespace, which conversion ignores
   (F&O 4.0, section 14.6.4). */
static bool element_map_is_xsi_attribute(const element_map_context *context,
                                         const sxml_node_description *description) {
    return element_map_namespace_of(context, description->node_name).index ==
           SXML_NAMESPACE_XML_SCHEMA_INSTANCE;
}

static int element_map_compare_names(const void *first, const void *second) {
    const uint32_t first_name = *(const uint32_t *)first;
    const uint32_t second_name = *(const uint32_t *)second;
    return (first_name > second_name) - (first_name < second_name);
}

/* An element whose shape is wanted. */
typedef struct element_map_shape_request {
    sxml_node_reference element;
    const sxml_node_description *description;
} element_map_shape_request;

static sxml_status element_map_describe_shape(element_map_conversion *conversion,
                                              element_map_shape_request request,
                                              element_map_shape *shape) {
    const element_map_context *context = conversion->context;
    *shape = (element_map_shape){.has_one_child_name = true, .has_distinct_child_names = true};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t attribute_number = 1;
         status == SXML_STATUS_OK && attribute_number <= request.description->attribute_count;
         attribute_number += 1) {
        sxml_node_description attribute;
        status = sxml_node_describe(
            (sxml_node_reference){.tree = request.element.tree,
                                  .node_index = {request.element.node_index.index +
                                                 attribute_number},
                                  .namespace_number = 0},
            &attribute);
        if (status == SXML_STATUS_OK && !element_map_is_xsi_attribute(context, &attribute)) {
            shape->has_attributes = true;
        }
    }
    conversion->child_names.element_count = 0;
    bool has_child = request.description->has_children;
    sxml_node_reference child = {.tree = request.element.tree,
                                 .node_index = request.description->first_child,
                                 .namespace_number = 0};
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = sxml_node_describe(child, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            if (shape->child_element_count > 0 &&
                child_description.node_name.index != shape->child_name.index) {
                shape->has_one_child_name = false;
            }
            shape->child_name = shape->child_element_count == 0 ? child_description.node_name
                                                                : shape->child_name;
            shape->child_element_count += 1;
            status = element_map_buffer_reserve(context, &conversion->child_names,
                                                sizeof(uint32_t));
            if (status == SXML_STATUS_OK) {
                ((uint32_t *)conversion->child_names.elements)[conversion->child_names
                                                                   .element_count] =
                    child_description.node_name.index;
                conversion->child_names.element_count += 1;
            }
        } else if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(child, &text);
            shape->has_text = true;
            if (!element_map_is_whitespace(text)) {
                shape->has_non_whitespace_text = true;
            }
        }
        has_child = child_description.has_next_sibling;
        child.node_index = child_description.next_sibling;
    }
    if (status == SXML_STATUS_OK && conversion->child_names.element_count > 1) {
        uint32_t *names = conversion->child_names.elements;
        qsort(names, conversion->child_names.element_count, sizeof(uint32_t),
              element_map_compare_names);
        for (size_t name_index = 1; name_index < conversion->child_names.element_count;
             name_index += 1) {
            if (names[name_index] == names[name_index - 1]) {
                shape->has_distinct_child_names = false;
            }
        }
    }
    if (shape->child_element_count == 0) {
        shape->has_one_child_name = false;
    }
    return status;
}

/* True when a layout can be used for an element of this shape (the
   "Errors" rows of F&O 4.0, section 14.6.1). */
typedef struct element_map_fit {
    element_map_layout layout;
    const element_map_plan_entry *entry;
} element_map_fit;

static bool element_map_layout_fits(element_map_fit fit, const element_map_shape *shape) {
    switch (fit.layout) {
    case SXML_ELEMENT_MAP_LAYOUT_EMPTY:
    case SXML_ELEMENT_MAP_LAYOUT_EMPTY_PLUS:
        return shape->child_element_count == 0 && !shape->has_non_whitespace_text;
    case SXML_ELEMENT_MAP_LAYOUT_SIMPLE:
    case SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS:
        return shape->child_element_count == 0;
    case SXML_ELEMENT_MAP_LAYOUT_LIST:
    case SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS:
        return !shape->has_non_whitespace_text &&
               (shape->child_element_count == 0 ||
                (shape->has_one_child_name && fit.entry != NULL &&
                 fit.entry->child_name.index == shape->child_name.index));
    case SXML_ELEMENT_MAP_LAYOUT_RECORD:
    case SXML_ELEMENT_MAP_LAYOUT_SEQUENCE:
        return !shape->has_non_whitespace_text;
    case SXML_ELEMENT_MAP_LAYOUT_NONE:
    case SXML_ELEMENT_MAP_LAYOUT_MIXED:
    case SXML_ELEMENT_MAP_LAYOUT_XML:
    case SXML_ELEMENT_MAP_LAYOUT_ERROR:
    case SXML_ELEMENT_MAP_LAYOUT_DEEP_SKIP:
        return true;
    }
    return true;
}

/* The layout F&O 4.0, section 14.6.2 chooses for a single element. */
static element_map_layout element_map_infer_layout(const element_map_shape *shape) {
    if (shape->child_element_count == 0) {
        if (!shape->has_text) {
            return shape->has_attributes ? SXML_ELEMENT_MAP_LAYOUT_EMPTY_PLUS
                                         : SXML_ELEMENT_MAP_LAYOUT_EMPTY;
        }
        return shape->has_attributes ? SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS
                                     : SXML_ELEMENT_MAP_LAYOUT_SIMPLE;
    }
    if (shape->has_non_whitespace_text) {
        return SXML_ELEMENT_MAP_LAYOUT_MIXED;
    }
    if (shape->has_one_child_name && shape->child_element_count > 1) {
        return shape->has_attributes ? SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS
                                     : SXML_ELEMENT_MAP_LAYOUT_LIST;
    }
    return shape->has_distinct_child_names ? SXML_ELEMENT_MAP_LAYOUT_RECORD
                                           : SXML_ELEMENT_MAP_LAYOUT_SEQUENCE;
}

/* The string value of an element: the item (owned by the caller) and
   its text. */
typedef struct element_map_string_value_result {
    sxml_item item;
    sxml_byte_view text;
} element_map_string_value_result;

static sxml_status element_map_string_value(const element_map_context *context,
                                            sxml_node_reference element,
                                            element_map_string_value_result *string_value) {
    sxml_status status =
        sxml_node_make_string_value(context->item_heap, element, &string_value->item);
    if (status == SXML_STATUS_OK && !element_map_item_text(string_value->item, &string_value->text)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    return status;
}

/* Uses a plan entry (or the fallback) for an element: deep-skip leaves it
   out, error is FOJS0008. */
static sxml_status element_map_use_entry(const element_map_context *context,
                                         const element_map_plan_entry *entry,
                                         element_slot *slot) {
    if (entry->layout == SXML_ELEMENT_MAP_LAYOUT_ERROR) {
        return element_map_report(context, &element_map_layout_error);
    }
    slot->layout = entry->layout;
    slot->content_type = entry->content_type;
    slot->has_child_name = entry->has_child_name;
    slot->child_name = entry->child_name;
    return SXML_STATUS_OK;
}

/* Chooses the layout of the element at slot (F&O 4.0, section 14.6.7). */
static sxml_status element_map_select_layout(element_map_conversion *conversion,
                                             element_map_shape_request request,
                                             element_slot *slot) {
    const element_map_context *context = conversion->context;
    element_map_shape shape;
    sxml_status status = element_map_describe_shape(conversion, request, &shape);
    const element_map_plan_entry *entry = NULL;
    if (status == SXML_STATUS_OK) {
        status = element_map_find_plan_entry(
            context,
            (element_map_name_query){.name = request.description->node_name, .is_attribute = false},
            &entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    slot->is_converted = true;
    if (entry != NULL && (entry->layout == SXML_ELEMENT_MAP_LAYOUT_DEEP_SKIP ||
                          entry->layout == SXML_ELEMENT_MAP_LAYOUT_ERROR ||
                          element_map_layout_fits((element_map_fit){entry->layout, entry},
                                                  &shape))) {
        return element_map_use_entry(context, entry, slot);
    }
    if (entry != NULL || context->has_fallback) {
        if (!context->has_fallback ||
            !element_map_layout_fits(
                (element_map_fit){context->fallback.layout, &context->fallback}, &shape)) {
            return element_map_report(context, &element_map_layout_error);
        }
        return element_map_use_entry(context, &context->fallback, slot);
    }
    slot->layout = element_map_infer_layout(&shape);
    slot->content_type = SXML_ELEMENT_MAP_CONTENT_TYPE_STRING;
    slot->has_child_name = shape.has_one_child_name;
    slot->child_name = shape.child_name;
    if (slot->layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE ||
        slot->layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS) {
        element_map_string_value_result string_value = {.item = {.representation = 0}, .text = {NULL, 0}};
        status = element_map_string_value(context, request.element, &string_value);
        const sxml_byte_view text = string_value.text;
        if (status == SXML_STATUS_OK) {
            element_map_type_evidence evidence = {.has_strings = false};
            element_map_add_evidence(&evidence, text);
            slot->content_type = element_map_infer_type(&evidence);
        }
        sxml_item_release(string_value.item);
    }
    return status;
}

/* ---------------------------------------------- Element-to-map: values */

/* The xml layout: the element serialized by the XML method without
   indentation or declaration (F&O 4.0, section 14.6.1.10). */
static sxml_status element_map_serialize(const element_map_context *context,
                                         sxml_node_reference element, sxml_sequence *value) {
    sxml_output_buffer *output = NULL;
    sxml_status status = sxml_output_buffer_create(
        context->allocator,
        &(sxml_output_buffer_configuration){.allocator = context->allocator,
                                            .maximum_byte_count = 0},
        &output);
    const sxml_item node_item = sxml_node_item_make(element);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            context->item_heap,
            &(sxml_serialization_request){
                .value = sxml_sequence_from_item(node_item),
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_XML},
                .error_record = context->call->error_record},
            sxml_output_buffer_as_output_stream(output));
    }
    sxml_item_release(node_item);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = element_map_make_text_item(
            context,
            (element_map_text_item){.type = SXML_TYPE_XS_STRING,
                                    .utf8 = sxml_output_buffer_get_bytes(output)},
            &item);
    }
    sxml_output_buffer_destroy(output);
    if (status == SXML_STATUS_OK) {
        *value = sxml_sequence_from_item(item);
    }
    return status;
}

/* What a child contributes to its parent's value. */
typedef enum element_map_part_kind {
    SXML_ELEMENT_MAP_PART_KIND_ELEMENT,
    SXML_ELEMENT_MAP_PART_KIND_TEXT,
    SXML_ELEMENT_MAP_PART_KIND_COMMENT,
    SXML_ELEMENT_MAP_PART_KIND_PROCESSING_INSTRUCTION,
} element_map_part_kind;

/* One child's part: for elements a key and the element's value; for the
   others the value of the member or entry. Owned. */
typedef struct element_map_part {
    element_map_part_kind part_kind;
    sxml_item key;
    sxml_sequence value;
} element_map_part;

/* The element being given a value. */
typedef struct element_map_element {
    sxml_node_reference element;
    const sxml_node_description *description;
    element_slot *slot;
} element_map_element;

static void element_map_release_parts(const element_map_context *context,
                                      element_map_buffer *parts) {
    element_map_part *part_array = parts->elements;
    for (size_t part_index = 0; part_index < parts->element_count; part_index += 1) {
        sxml_item_release(part_array[part_index].key);
        sxml_sequence_release(part_array[part_index].value);
    }
    element_map_buffer_release(context, parts);
}

/* The value of a comment or processing-instruction child in the mixed
   and sequence layouts (F&O 4.0, section 14.6.1.9). */
static sxml_status element_map_make_node_part(const element_map_context *context,
                                              element_map_shape_request request,
                                              element_map_part *part) {
    const sxml_node_reference child = request.element;
    const sxml_node_description *description = request.description;
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_node_get_content(child, &content);
    sxml_item content_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = element_map_make_text_item(
            context, (element_map_text_item){.type = SXML_TYPE_XS_STRING, .utf8 = content},
            &content_item);
    }
    sxml_item key = {.representation = 0};
    if (description->node_kind == SXML_NODE_KIND_COMMENT) {
        if (status == SXML_STATUS_OK) {
            status = element_map_make_literal_key(context, SXML_BYTE_VIEW_FROM_LITERAL("#comment"),
                                                  &key);
        }
        if (status == SXML_STATUS_OK) {
            part->part_kind = SXML_ELEMENT_MAP_PART_KIND_COMMENT;
            return element_map_make_single_entry_map(
                context,
                (element_map_entry_addition){.key = key,
                                             .value = sxml_sequence_from_item(content_item)},
                &part->value);
        }
        sxml_item_release(content_item);
        return status;
    }
    sxml_name_parts target;
    sxml_item target_item = {.representation = 0};
    sxml_item target_key = {.representation = 0};
    sxml_item data_key = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(context->name_pool, description->node_name, &target);
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_make_text_item(
            context, (element_map_text_item){.type = SXML_TYPE_XS_NCNAME, .utf8 = target.local_name},
            &target_item);
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_make_literal_key(context, SXML_BYTE_VIEW_FROM_LITERAL("#target"),
                                              &target_key);
    }
    if (status == SXML_STATUS_OK) {
        status =
            element_map_make_literal_key(context, SXML_BYTE_VIEW_FROM_LITERAL("#data"), &data_key);
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_make_literal_key(
            context, SXML_BYTE_VIEW_FROM_LITERAL("#processing-instruction"), &key);
    }
    sxml_item instruction = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        const sxml_map_entry entries[] = {
            {.key = target_key, .value = sxml_sequence_from_item(target_item)},
            {.key = data_key, .value = sxml_sequence_from_item(content_item)},
        };
        status = sxml_map_make(context->item_heap,
                               &(sxml_map_request){.map_entries = {entries, 2},
                                                   .duplicate_key_policy =
                                                       SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                                                   .error_record = NULL},
                               &instruction);
    }
    sxml_item_release(target_item);
    sxml_item_release(content_item);
    sxml_item_release(target_key);
    sxml_item_release(data_key);
    if (status != SXML_STATUS_OK) {
        sxml_item_release(key);
        return status;
    }
    part->part_kind = SXML_ELEMENT_MAP_PART_KIND_PROCESSING_INSTRUCTION;
    return element_map_make_single_entry_map(
        context,
        (element_map_entry_addition){.key = key, .value = sxml_sequence_from_item(instruction)},
        &part->value);
}

/* Collects the parts of an element's children: converted elements (their
   values are taken from their slots) and, for the mixed and sequence
   layouts, text (mixed only), comments and processing instructions. */
static sxml_status element_map_collect_parts(element_map_conversion *conversion,
                                             element_map_element target,
                                             element_map_buffer *parts) {
    const element_map_context *context = conversion->context;
    const element_map_layout layout = target.slot->layout;
    const bool keeps_nodes =
        layout == SXML_ELEMENT_MAP_LAYOUT_MIXED || layout == SXML_ELEMENT_MAP_LAYOUT_SEQUENCE;
    const sxml_namespace_uri_identifier namespace_uri =
        element_map_namespace_of(context, target.description->node_name);
    sxml_status status = SXML_STATUS_OK;
    bool has_child = target.description->has_children;
    sxml_node_reference child = {.tree = target.element.tree,
                                 .node_index = target.description->first_child,
                                 .namespace_number = 0};
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description description;
        status = sxml_node_describe(child, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        has_child = description.has_next_sibling;
        const sxml_node_reference current = child;
        child.node_index = description.next_sibling;
        element_map_part part = {.part_kind = SXML_ELEMENT_MAP_PART_KIND_TEXT,
                                 .key = {.representation = 0},
                                 .value = sxml_sequence_empty()};
        if (description.node_kind == SXML_NODE_KIND_ELEMENT) {
            element_slot *child_slot =
                &conversion->slots[current.node_index.index - conversion->root.node_index.index];
            if (!child_slot->has_value) {
                continue;
            }
            part.part_kind = SXML_ELEMENT_MAP_PART_KIND_ELEMENT;
            status = element_map_make_key(
                context,
                &(element_map_key_request){.name = description.node_name,
                                           .namespace_prefix = description.namespace_prefix,
                                           .is_attribute = false,
                                           .is_top = false,
                                           .parent_namespace = namespace_uri},
                &part.key);
            if (status == SXML_STATUS_OK) {
                part.value = child_slot->value;
                child_slot->value = sxml_sequence_empty();
                child_slot->has_value = false;
            }
        } else if (description.node_kind == SXML_NODE_KIND_TEXT) {
            if (layout != SXML_ELEMENT_MAP_LAYOUT_MIXED) {
                continue;
            }
            sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(current, &text);
            sxml_item item = {.representation = 0};
            if (status == SXML_STATUS_OK) {
                status = element_map_make_text_item(
                    context,
                    (element_map_text_item){.type = SXML_TYPE_XS_UNTYPED_ATOMIC, .utf8 = text},
                    &item);
            }
            part.value = sxml_sequence_from_item(item);
        } else if (keeps_nodes && (description.node_kind == SXML_NODE_KIND_COMMENT ||
                                   description.node_kind ==
                                       SXML_NODE_KIND_PROCESSING_INSTRUCTION)) {
            status = element_map_make_node_part(
                context,
                (element_map_shape_request){.element = current, .description = &description},
                &part);
        } else {
            continue;
        }
        if (status == SXML_STATUS_OK) {
            status = element_map_buffer_reserve(context, parts, sizeof(element_map_part));
        }
        if (status != SXML_STATUS_OK) {
            sxml_item_release(part.key);
            sxml_sequence_release(part.value);
            break;
        }
        ((element_map_part *)parts->elements)[parts->element_count] = part;
        parts->element_count += 1;
    }
    return status;
}

/* Attributes to collect, and the child keys a marker must not meet. */
typedef struct element_map_attribute_request {
    element_map_element target;
    sxml_byte_view attribute_marker;
    const element_map_entry_list *child_keys;
    /* Receives the attributes. */
    element_map_entry_list *list;
} element_map_attribute_request;

/* Adds the element's attributes (not those in the xsi namespace, not
   those whose plan type is skip) to list, and sets *has_conflict when a
   key equals a child key (F&O 4.0, section 14.6.8). */
static sxml_status element_map_collect_attributes(const element_map_context *context,
                                                  element_map_attribute_request request,
                                                  element_map_answer *has_conflict) {
    element_map_entry_list *list = request.list;
    has_conflict->is_true = false;
    sxml_status status = SXML_STATUS_OK;
    const sxml_node_reference element = request.target.element;
    for (uint32_t attribute_number = 1;
         status == SXML_STATUS_OK &&
         attribute_number <= request.target.description->attribute_count;
         attribute_number += 1) {
        const sxml_node_reference attribute = {
            .tree = element.tree,
            .node_index = {element.node_index.index + attribute_number},
            .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(attribute, &description);
        if (status != SXML_STATUS_OK || element_map_is_xsi_attribute(context, &description)) {
            continue;
        }
        const element_map_plan_entry *entry = NULL;
        status = element_map_find_plan_entry(
            context, (element_map_name_query){.name = description.node_name, .is_attribute = true},
            &entry);
        const element_map_content_type content_type =
            entry == NULL ? SXML_ELEMENT_MAP_CONTENT_TYPE_STRING : entry->content_type;
        if (status != SXML_STATUS_OK || content_type == SXML_ELEMENT_MAP_CONTENT_TYPE_SKIP) {
            continue;
        }
        sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(attribute, &text);
        element_map_entry_addition addition = {.key = {.representation = 0},
                                               .value = sxml_sequence_empty()};
        if (status == SXML_STATUS_OK) {
            status = element_map_make_content(
                context,
                (element_map_content_request){.text = text, .content_type = content_type},
                &addition.value);
        }
        if (status == SXML_STATUS_OK) {
            status = element_map_make_key(
                context,
                &(element_map_key_request){.name = description.node_name,
                                           .namespace_prefix = description.namespace_prefix,
                                           .is_attribute = true,
                                           .attribute_marker = request.attribute_marker},
                &addition.key);
        }
        size_t position = SIZE_MAX;
        if (status == SXML_STATUS_OK) {
            status = element_map_entry_list_find(request.child_keys, addition.key, &position);
        }
        if (position != SIZE_MAX) {
            has_conflict->is_true = true;
        }
        if (status == SXML_STATUS_OK) {
            status = element_map_entry_list_add(context, list, addition);
        } else {
            sxml_item_release(addition.key);
            sxml_sequence_release(addition.value);
        }
    }
    return status;
}

/* The content key of a map that has the entries of list: the content-key
   option with as many "#" in front as make it unique (F&O 4.0, 14.6.14). */
static sxml_status element_map_make_content_key(const element_map_context *context,
                                                const element_map_entry_list *list,
                                                sxml_item *key) {
    sxml_library_text text = {.item_heap = context->item_heap};
    sxml_status status = sxml_function_library_text_append(&text, context->content_key);
    element_map_answer is_found = {.is_true = true};
    while (status == SXML_STATUS_OK) {
        status = element_map_entry_list_has_text(
            context,
            (element_map_text_query){.list = list, .text = sxml_function_library_text_view(&text)},
            &is_found);
        if (status != SXML_STATUS_OK || !is_found.is_true) {
            break;
        }
        sxml_library_text longer = {.item_heap = context->item_heap};
        status = sxml_function_library_text_append(&longer, SXML_BYTE_VIEW_FROM_LITERAL("#"));
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&longer,
                                                       sxml_function_library_text_view(&text));
        }
        sxml_function_library_text_release(&text);
        text = longer;
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_make_literal_key(context, sxml_function_library_text_view(&text), key);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* The array of the values of parts, or for the mixed and sequence layouts
   of all parts with the attributes of attributes first. */
typedef struct element_map_array_request {
    element_map_buffer *parts;
    element_map_entry_list *attributes;
    bool has_all_parts;
} element_map_array_request;

static sxml_status element_map_make_member_array(const element_map_context *context,
                                                 element_map_array_request request,
                                                 sxml_sequence *array_value) {
    element_map_buffer members = {NULL, 0, 0};
    sxml_status status = SXML_STATUS_OK;
    if (request.attributes != NULL) {
        const element_map_pending_entry *entries = request.attributes->entries.elements;
        for (size_t entry_index = 0;
             status == SXML_STATUS_OK && entry_index < request.attributes->entries.element_count;
             entry_index += 1) {
            sxml_sequence value = sxml_sequence_empty();
            status = element_map_entry_value(context, &entries[entry_index], &value);
            sxml_item_retain(entries[entry_index].key);
            sxml_sequence member = sxml_sequence_empty();
            if (status == SXML_STATUS_OK) {
                status = element_map_make_single_entry_map(
                    context,
                    (element_map_entry_addition){.key = entries[entry_index].key, .value = value},
                    &member);
            } else {
                sxml_item_release(entries[entry_index].key);
            }
            if (status == SXML_STATUS_OK) {
                status = element_map_buffer_reserve(context, &members, sizeof(sxml_sequence));
            }
            if (status == SXML_STATUS_OK) {
                ((sxml_sequence *)members.elements)[members.element_count] = member;
                members.element_count += 1;
            } else {
                sxml_sequence_release(member);
            }
        }
    }
    element_map_part *parts = request.parts->elements;
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < request.parts->element_count;
         part_index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        element_map_part *part = &parts[part_index];
        if (part->part_kind == SXML_ELEMENT_MAP_PART_KIND_ELEMENT && request.has_all_parts) {
            status = element_map_make_single_entry_map(
                context, (element_map_entry_addition){.key = part->key, .value = part->value},
                &member);
            part->key = (sxml_item){.representation = 0};
        } else {
            member = part->value;
        }
        part->value = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = element_map_buffer_reserve(context, &members, sizeof(sxml_sequence));
        }
        if (status == SXML_STATUS_OK) {
            ((sxml_sequence *)members.elements)[members.element_count] = member;
            members.element_count += 1;
        } else {
            sxml_sequence_release(member);
        }
    }
    sxml_item array = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(
            context->item_heap,
            (sxml_sequence_view){.sequences = members.elements,
                                 .sequence_count = members.element_count},
            &array);
    }
    for (size_t member_index = 0; member_index < members.element_count; member_index += 1) {
        sxml_sequence_release(((sxml_sequence *)members.elements)[member_index]);
    }
    element_map_buffer_release(context, &members);
    if (status == SXML_STATUS_OK) {
        *array_value = sxml_sequence_from_item(array);
    }
    return status;
}

/* Gives an element of a layout with attributes or children its value
   (F&O 4.0, sections 14.6.1.2 and 14.6.1.4 to 14.6.1.9). */
static sxml_status element_map_build_structure(element_map_conversion *conversion,
                                               element_map_element target,
                                               sxml_sequence *value) {
    const element_map_context *context = conversion->context;
    const element_map_layout layout = target.slot->layout;
    element_map_buffer parts = {NULL, 0, 0};
    element_map_entry_list child_keys = {.entries = {NULL, 0, 0}, .has_index = false};
    element_map_entry_list attributes = {.entries = {NULL, 0, 0}, .has_index = false};
    sxml_status status = element_map_collect_parts(conversion, target, &parts);
    element_map_part *part_array = parts.elements;
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < parts.element_count;
         part_index += 1) {
        if (part_array[part_index].part_kind != SXML_ELEMENT_MAP_PART_KIND_ELEMENT) {
            continue;
        }
        sxml_item_retain(part_array[part_index].key);
        sxml_sequence_retain(part_array[part_index].value);
        status = element_map_entry_list_add(
            context, &child_keys,
            (element_map_entry_addition){.key = part_array[part_index].key,
                                         .value = part_array[part_index].value});
    }
    element_map_answer has_conflict = {.is_true = false};
    if (status == SXML_STATUS_OK && layout != SXML_ELEMENT_MAP_LAYOUT_LIST) {
        status = element_map_collect_attributes(
            context,
            (element_map_attribute_request){.target = target,
                                            .attribute_marker = context->attribute_marker,
                                            .child_keys = &child_keys,
                                            .list = &attributes},
            &has_conflict);
    }
    if (status == SXML_STATUS_OK && has_conflict.is_true) {
        /* A conflict reverts the marker to "@" (F&O 4.0, section 14.6.8). */
        element_map_entry_list_release(context, &attributes);
        status = element_map_collect_attributes(
            context,
            (element_map_attribute_request){.target = target,
                                            .attribute_marker = SXML_BYTE_VIEW_FROM_LITERAL("@"),
                                            .child_keys = &child_keys,
                                            .list = &attributes},
            &has_conflict);
    }
    if (status != SXML_STATUS_OK) {
        /* Nothing more to do. */
    } else if (layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS) {
        element_map_entry_addition addition = {.key = {.representation = 0},
                                               .value = sxml_sequence_empty()};
        element_map_string_value_result string_value = {.item = {.representation = 0}, .text = {NULL, 0}};
        status = element_map_string_value(context, target.element, &string_value);
        const sxml_byte_view text = string_value.text;
        if (status == SXML_STATUS_OK) {
            status = element_map_make_content(
                context,
                (element_map_content_request){.text = text,
                                              .content_type = target.slot->content_type},
                &addition.value);
        }
        sxml_item_release(string_value.item);
        if (status == SXML_STATUS_OK) {
            status = element_map_make_content_key(context, &attributes, &addition.key);
        }
        if (status == SXML_STATUS_OK) {
            status = element_map_entry_list_add(context, &attributes, addition);
        } else {
            sxml_sequence_release(addition.value);
        }
    } else if (layout == SXML_ELEMENT_MAP_LAYOUT_RECORD) {
        for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < parts.element_count;
             part_index += 1) {
            status = element_map_entry_list_add(
                context, &attributes,
                (element_map_entry_addition){.key = part_array[part_index].key,
                                             .value = part_array[part_index].value});
            part_array[part_index].key = (sxml_item){.representation = 0};
            part_array[part_index].value = sxml_sequence_empty();
        }
    } else if (layout == SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS) {
        /* The content property is named after the children, or after the
           plan's child name when there are none. */
        element_map_entry_addition addition = {.key = {.representation = 0},
                                               .value = sxml_sequence_empty()};
        if (parts.element_count > 0) {
            addition.key = part_array[0].key;
            sxml_item_retain(addition.key);
        } else if (target.slot->has_child_name) {
            status = element_map_make_key(
                context,
                &(element_map_key_request){
                    .name = target.slot->child_name,
                    .namespace_prefix = {.index = 0},
                    .is_attribute = false,
                    .is_top = false,
                    .parent_namespace =
                        element_map_namespace_of(context, target.description->node_name)},
                &addition.key);
        }
        if (status == SXML_STATUS_OK && addition.key.representation != 0) {
            status = element_map_make_member_array(
                context,
                (element_map_array_request){.parts = &parts, .attributes = NULL,
                                            .has_all_parts = false},
                &addition.value);
            if (status == SXML_STATUS_OK) {
                status = element_map_entry_list_add(context, &attributes, addition);
            } else {
                sxml_item_release(addition.key);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        if (layout == SXML_ELEMENT_MAP_LAYOUT_LIST || layout == SXML_ELEMENT_MAP_LAYOUT_SEQUENCE ||
            layout == SXML_ELEMENT_MAP_LAYOUT_MIXED) {
            status = element_map_make_member_array(
                context,
                (element_map_array_request){.parts = &parts,
                                            .attributes = layout == SXML_ELEMENT_MAP_LAYOUT_LIST
                                                              ? NULL
                                                              : &attributes,
                                            .has_all_parts = layout != SXML_ELEMENT_MAP_LAYOUT_LIST},
                value);
        } else {
            status = element_map_entry_list_finish(context, &attributes, value);
        }
    }
    element_map_entry_list_release(context, &attributes);
    element_map_entry_list_release(context, &child_keys);
    element_map_release_parts(context, &parts);
    return status;
}

/* Gives the element at a converted slot its value. */
static sxml_status element_map_build_value(element_map_conversion *conversion,
                                           element_map_element target) {
    const element_map_context *context = conversion->context;
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = SXML_STATUS_OK;
    switch (target.slot->layout) {
    case SXML_ELEMENT_MAP_LAYOUT_EMPTY: {
        /* F&O 4.0, section 14.6.1.1: the zero-length xs:string. */
        sxml_item item = {.representation = 0};
        status = element_map_make_literal_key(context, (sxml_byte_view){.bytes = NULL,
                                                                         .byte_count = 0},
                                              &item);
        value = sxml_sequence_from_item(item);
        break;
    }
    case SXML_ELEMENT_MAP_LAYOUT_SIMPLE: {
        element_map_string_value_result string_value = {.item = {.representation = 0}, .text = {NULL, 0}};
        status = element_map_string_value(context, target.element, &string_value);
        const sxml_byte_view text = string_value.text;
        if (status == SXML_STATUS_OK) {
            status = element_map_make_content(
                context,
                (element_map_content_request){.text = text,
                                              .content_type = target.slot->content_type},
                &value);
        }
        sxml_item_release(string_value.item);
        break;
    }
    case SXML_ELEMENT_MAP_LAYOUT_XML:
        status = element_map_serialize(context, target.element, &value);
        break;
    case SXML_ELEMENT_MAP_LAYOUT_NONE:
    case SXML_ELEMENT_MAP_LAYOUT_EMPTY_PLUS:
    case SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS:
    case SXML_ELEMENT_MAP_LAYOUT_LIST:
    case SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS:
    case SXML_ELEMENT_MAP_LAYOUT_RECORD:
    case SXML_ELEMENT_MAP_LAYOUT_SEQUENCE:
    case SXML_ELEMENT_MAP_LAYOUT_MIXED:
    case SXML_ELEMENT_MAP_LAYOUT_ERROR:
    case SXML_ELEMENT_MAP_LAYOUT_DEEP_SKIP:
        status = element_map_build_structure(conversion, target, &value);
        break;
    }
    if (status == SXML_STATUS_OK) {
        target.slot->value = value;
        target.slot->has_value = true;
    }
    return status;
}

/* Converts the subtree of conversion->root: layouts in document order,
   values in reverse document order (D71: no recursion). */
static sxml_status element_map_convert(element_map_conversion *conversion) {
    const element_map_context *context = conversion->context;
    const uint32_t root_index = conversion->root.node_index.index;
    sxml_status status = SXML_STATUS_OK;
    for (size_t slot_index = 0; status == SXML_STATUS_OK && slot_index < conversion->slot_count;
         slot_index += 1) {
        const sxml_node_reference node = {.tree = conversion->root.tree,
                                          .node_index = {root_index + (uint32_t)slot_index},
                                          .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK || description.node_kind != SXML_NODE_KIND_ELEMENT) {
            continue;
        }
        if (slot_index > 0) {
            const element_slot *parent = &conversion->slots[description.parent.index - root_index];
            if (!parent->is_converted || !element_map_layout_converts_children(parent->layout)) {
                continue;
            }
        }
        status = element_map_select_layout(
            conversion, (element_map_shape_request){.element = node, .description = &description},
            &conversion->slots[slot_index]);
    }
    for (size_t slot_number = conversion->slot_count;
         status == SXML_STATUS_OK && slot_number > 0; slot_number -= 1) {
        element_slot *slot = &conversion->slots[slot_number - 1];
        if (!slot->is_converted || slot->layout == SXML_ELEMENT_MAP_LAYOUT_DEEP_SKIP) {
            continue;
        }
        const sxml_node_reference node = {.tree = conversion->root.tree,
                                          .node_index = {root_index + (uint32_t)(slot_number - 1)},
                                          .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status == SXML_STATUS_OK) {
            status = element_map_build_value(
                conversion,
                (element_map_element){.element = node, .description = &description, .slot = slot});
        }
    }
    (void)context;
    return status;
}

/* The element an argument of type (document-node(*) | element())?
   stands for: an element, or the one element child of a document node
   without text children; XPTY0004 otherwise. */
static sxml_status element_map_input_element(const element_map_context *context,
                                             sxml_item item, sxml_node_reference *element) {
    sxml_node_reference node;
    sxml_node_description description;
    sxml_status status = sxml_item_get_node(item, &node);
    if (status != SXML_STATUS_OK) {
        return element_map_report(context, &element_map_type_error);
    }
    status = sxml_node_describe(node, &description);
    if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
        *element = node;
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK || description.node_kind != SXML_NODE_KIND_DOCUMENT) {
        return status != SXML_STATUS_OK ? status
                                        : element_map_report(context, &element_map_type_error);
    }
    size_t element_count = 0;
    bool has_child = description.has_children;
    sxml_node_reference child = {.tree = node.tree,
                                 .node_index = description.first_child,
                                 .namespace_number = 0};
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = sxml_node_describe(child, &child_description);
        if (status == SXML_STATUS_OK && child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            element_count += 1;
            *element = child;
        } else if (status == SXML_STATUS_OK &&
                   child_description.node_kind == SXML_NODE_KIND_TEXT) {
            element_count = 2;
        }
        has_child = status == SXML_STATUS_OK && child_description.has_next_sibling;
        child.node_index = child_description.next_sibling;
    }
    if (status == SXML_STATUS_OK && element_count != 1) {
        return element_map_report(context, &element_map_type_error);
    }
    return status;
}

static sxml_allocator element_map_call_allocator(const sxml_function_call *call) {
    return call->dynamic_context != NULL ? call->dynamic_context->allocator
                                         : sxml_item_heap_as_allocator(call->item_heap);
}

static element_map_context element_map_start_context(const sxml_function_call *call) {
    element_map_context context;
    memset(&context, 0, sizeof(context));
    context.call = call;
    context.item_heap = call->item_heap;
    context.name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    context.allocator = element_map_call_allocator(call);
    return context;
}

/* fn:element-to-map($element as (document-node(*) | element())?,
   $options as map(*)? := {}) as map(xs:string, item()?)? (F&O 4.0,
   section 14.6.14). */
static sxml_status call_element_to_map(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    element_map_context context = element_map_start_context(call);
    sxml_status status = element_map_read_options(&context);
    if (status != SXML_STATUS_OK || element_map_is_empty(input)) {
        element_map_context_release(&context);
        *result = sxml_sequence_empty();
        return status;
    }
    element_map_conversion conversion = {.context = &context,
                                         .slots = NULL,
                                         .slot_count = 0,
                                         .child_names = {NULL, 0, 0}};
    sxml_node_description description;
    status = element_map_input_element(&context, input.content, &conversion.root);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(conversion.root, &description);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        conversion.slot_count = (size_t)description.last_descendant.index -
                                conversion.root.node_index.index + 1;
        /* One step per node of the subtree (D89). */
        status = sxml_function_library_charge_steps(call, conversion.slot_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            context.allocator,
            (sxml_allocation_request){.byte_count = conversion.slot_count * sizeof(element_slot),
                                      .alignment = alignof(element_slot)},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        conversion.slots = memory;
        memset(conversion.slots, 0, conversion.slot_count * sizeof(element_slot));
        status = element_map_convert(&conversion);
    }
    *result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && conversion.slots[0].has_value) {
        element_map_entry_addition addition = {.key = {.representation = 0},
                                               .value = conversion.slots[0].value};
        conversion.slots[0].has_value = false;
        status = element_map_make_key(
            &context,
            &(element_map_key_request){.name = description.node_name,
                                       .namespace_prefix = description.namespace_prefix,
                                       .is_attribute = false,
                                       .is_top = true},
            &addition.key);
        if (status == SXML_STATUS_OK) {
            status = element_map_make_single_entry_map(&context, addition, result);
        } else {
            sxml_sequence_release(addition.value);
        }
    }
    for (size_t slot_index = 0; slot_index < conversion.slot_count && conversion.slots != NULL;
         slot_index += 1) {
        if (conversion.slots[slot_index].has_value) {
            sxml_sequence_release(conversion.slots[slot_index].value);
        }
    }
    sxml_allocator_release(context.allocator, memory);
    element_map_buffer_release(&context, &conversion.child_names);
    element_map_context_release(&context);
    return status;
}

/* ------------------------------------------------ Element-to-map-plan */

/* What the elements or attributes of one name are like (F&O 4.0, 14.6.2). */
typedef struct element_map_group {
    sxml_name_identifier name;
    bool is_attribute;
    size_t element_count;
    size_t nilled_count;
    bool has_child_element;
    bool has_text;
    bool has_non_whitespace_text;
    bool has_attributes;
    bool has_child_name;
    bool has_different_child_names;
    bool has_second_child;
    bool has_repeated_child_name;
    sxml_name_identifier child_name;
    element_map_type_evidence evidence;
} element_map_group;

/* The groups found so far and an XDM map from plan key to position. */
typedef struct element_map_grouping {
    element_map_buffer groups;
    sxml_item index;
} element_map_grouping;

/* The group of a name, made when new; writes its position. */
typedef struct element_map_group_query {
    element_map_grouping *grouping;
    sxml_name_identifier name;
    bool is_attribute;
} element_map_group_query;

static sxml_status element_map_find_group(const element_map_context *context,
                                          element_map_group_query query, size_t *position) {
    element_map_grouping *grouping = query.grouping;
    const sxml_item key = element_map_plan_key((element_map_name_query){.name = query.name, .is_attribute = query.is_attribute});
    sxml_map_lookup lookup;
    sxml_status status = sxml_map_find(grouping->index, key, &lookup);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && lookup.is_found) {
        status = sxml_atomic_item_get_datum(lookup.entry.value.content, &datum);
        *position = (size_t)datum.datum_details.integer;
        return status;
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_buffer_reserve(context, &grouping->groups, sizeof(element_map_group));
    }
    sxml_item updated = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_put(
            context->item_heap,
            &(sxml_map_update){.map = grouping->index,
                               .key = key,
                               .value = sxml_sequence_from_item(sxml_atomic_item_from_integer(
                                   (int64_t)grouping->groups.element_count))},
            &updated);
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_release(grouping->index);
        grouping->index = updated;
        *position = grouping->groups.element_count;
        element_map_group *group = &((element_map_group *)grouping->groups.elements)[*position];
        memset(group, 0, sizeof(*group));
        group->name = query.name;
        group->is_attribute = query.is_attribute;
        grouping->groups.element_count += 1;
    }
    return status;
}

/* True for an element with xsi:nil="true" (F&O 4.0, section 14.6.4). */
static sxml_status element_map_is_nilled(const element_map_context *context,
                                         element_map_shape_request request, element_map_answer *is_nilled) {
    is_nilled->is_true = false;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t attribute_number = 1;
         status == SXML_STATUS_OK && attribute_number <= request.description->attribute_count;
         attribute_number += 1) {
        const sxml_node_reference attribute = {
            .tree = request.element.tree,
            .node_index = {request.element.node_index.index + attribute_number},
            .namespace_number = 0};
        sxml_node_description description;
        sxml_name_parts parts;
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_describe(attribute, &description);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(context->name_pool, description.node_name, &parts);
        }
        if (status == SXML_STATUS_OK &&
            parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA_INSTANCE &&
            sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("nil"))) {
            status = sxml_node_get_content(attribute, &value);
            value = sxml_xml_text_trim_whitespace(value);
            is_nilled->is_true = sxml_byte_view_equals(value, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
                         sxml_byte_view_equals(value, SXML_BYTE_VIEW_FROM_LITERAL("1"));
        }
    }
    return status;
}

/* Adds one element and its attributes to the grouping. */
static sxml_status element_map_add_to_grouping(element_map_conversion *conversion,
                                               element_map_shape_request request,
                                               element_map_grouping *grouping) {
    const element_map_context *context = conversion->context;
    size_t position = 0;
    sxml_status status = element_map_find_group(
        context,
        (element_map_group_query){
            .grouping = grouping, .name = request.description->node_name, .is_attribute = false},
        &position);
    element_map_answer is_nilled = {.is_true = false};
    if (status == SXML_STATUS_OK) {
        status = element_map_is_nilled(context, request, &is_nilled);
    }
    element_map_shape shape;
    if (status == SXML_STATUS_OK && !is_nilled.is_true) {
        status = element_map_describe_shape(conversion, request, &shape);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    element_map_group *group = &((element_map_group *)grouping->groups.elements)[position];
    if (is_nilled.is_true) {
        group->nilled_count += 1;
    } else {
        group->element_count += 1;
        group->has_child_element = group->has_child_element || shape.child_element_count > 0;
        group->has_text = group->has_text || shape.has_text;
        group->has_non_whitespace_text =
            group->has_non_whitespace_text || shape.has_non_whitespace_text;
        group->has_attributes = group->has_attributes || shape.has_attributes;
        group->has_second_child = group->has_second_child || shape.child_element_count > 1;
        group->has_repeated_child_name =
            group->has_repeated_child_name || !shape.has_distinct_child_names;
        if (shape.child_element_count > 0) {
            if (!group->has_child_name) {
                group->has_child_name = true;
                group->child_name = shape.child_name;
            }
            if (!shape.has_one_child_name || shape.child_name.index != group->child_name.index) {
                group->has_different_child_names = true;
            }
        } else {
            element_map_string_value_result string_value = {.item = {.representation = 0}, .text = {NULL, 0}};
            status = element_map_string_value(context, request.element, &string_value);
            const sxml_byte_view text = string_value.text;
            if (status == SXML_STATUS_OK) {
                element_map_add_evidence(&group->evidence, text);
            }
            sxml_item_release(string_value.item);
        }
    }
    for (uint32_t attribute_number = 1;
         status == SXML_STATUS_OK && attribute_number <= request.description->attribute_count;
         attribute_number += 1) {
        const sxml_node_reference attribute = {
            .tree = request.element.tree,
            .node_index = {request.element.node_index.index + attribute_number},
            .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(attribute, &description);
        if (status != SXML_STATUS_OK || element_map_is_xsi_attribute(context, &description)) {
            continue;
        }
        status = element_map_find_group(
            context,
            (element_map_group_query){
                .grouping = grouping, .name = description.node_name, .is_attribute = true},
            &position);
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_node_get_content(attribute, &value);
        }
        if (status == SXML_STATUS_OK) {
            element_map_add_evidence(
                &((element_map_group *)grouping->groups.elements)[position].evidence, value);
        }
    }
    return status;
}

/* The layout of an element group (F&O 4.0, sections 14.6.2 and 14.6.4). */
static element_map_layout element_map_group_layout(const element_map_group *group) {
    if (group->element_count == 0) {
        return SXML_ELEMENT_MAP_LAYOUT_MIXED;
    }
    const element_map_shape shape = {
        .child_element_count = group->has_child_element ? (group->has_second_child ? 2 : 1) : 0,
        .has_text = group->has_text,
        .has_non_whitespace_text = group->has_non_whitespace_text,
        .has_attributes = group->has_attributes,
        .has_one_child_name = group->has_child_name && !group->has_different_child_names,
        .has_distinct_child_names = !group->has_repeated_child_name,
        .child_name = group->child_name};
    return element_map_infer_layout(&shape);
}

/* "local" or "Q{uri}local", after prefix_text. */
typedef struct element_map_plan_key_text {
    sxml_name_identifier name;
    sxml_byte_view prefix_text;
} element_map_plan_key_text;

static sxml_status element_map_make_plan_name(const element_map_context *context,
                                              element_map_plan_key_text request,
                                              sxml_sequence *value) {
    sxml_name_parts parts;
    sxml_library_text text = {.item_heap = context->item_heap};
    sxml_status status = sxml_name_pool_get_name_parts(context->name_pool, request.name, &parts);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, request.prefix_text);
    }
    if (status == SXML_STATUS_OK && parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        status = sxml_name_pool_get_namespace_uri(context->name_pool, parts.namespace_uri,
                                                  &namespace_uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&text, namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("}"));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, parts.local_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(context->item_heap,
                                                   sxml_function_library_text_view(&text), value);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* The plan record of one group, owned; empty for an attribute of type
   string, which the plan leaves out. */
static sxml_status element_map_make_plan_record(const element_map_context *context,
                                                const element_map_group *group,
                                                sxml_sequence *record) {
    const element_map_content_type content_type = element_map_infer_type(&group->evidence);
    sxml_sequence values[3] = {sxml_sequence_empty(), sxml_sequence_empty(),
                               sxml_sequence_empty()};
    sxml_item keys[3] = {{.representation = 0}, {.representation = 0}, {.representation = 0}};
    static const char *const field_names[] = {"layout", "child", "type"};
    size_t field_count = group->is_attribute ? 1 : 3;
    sxml_status status = SXML_STATUS_OK;
    *record = sxml_sequence_empty();
    if (group->is_attribute && content_type == SXML_ELEMENT_MAP_CONTENT_TYPE_STRING) {
        return SXML_STATUS_OK;
    }
    const element_map_layout layout = element_map_group_layout(group);
    const bool is_simple = layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE ||
                           layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS;
    const bool is_list =
        layout == SXML_ELEMENT_MAP_LAYOUT_LIST || layout == SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS;
    for (size_t field_index = 0; status == SXML_STATUS_OK && field_index < field_count;
         field_index += 1) {
        const size_t field = group->is_attribute ? 2 : field_index;
        status = element_map_make_literal_key(
            context, sxml_byte_view_from_c_string(field_names[field]), &keys[field_index]);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (field == 0) {
            status = sxml_function_library_make_string(
                context->item_heap, sxml_byte_view_from_c_string(element_map_layout_names[layout]),
                &values[field_index]);
        } else if (field == 1 && is_list) {
            status = element_map_make_plan_name(
                context,
                (element_map_plan_key_text){.name = group->child_name,
                                            .prefix_text = {.bytes = NULL, .byte_count = 0}},
                &values[field_index]);
        } else if (field == 2 && (group->is_attribute || is_simple) &&
                   content_type != SXML_ELEMENT_MAP_CONTENT_TYPE_STRING) {
            status = sxml_function_library_make_string(
                context->item_heap,
                sxml_byte_view_from_c_string(element_map_content_type_names[content_type]),
                &values[field_index]);
        }
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        const sxml_map_entry entries[3] = {{.key = keys[0], .value = values[0]},
                                           {.key = keys[1], .value = values[1]},
                                           {.key = keys[2], .value = values[2]}};
        status = sxml_map_make(context->item_heap,
                               &(sxml_map_request){.map_entries = {entries, field_count},
                                                   .duplicate_key_policy =
                                                       SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                                                   .error_record = NULL},
                               &map);
    }
    for (size_t field_index = 0; field_index < 3; field_index += 1) {
        sxml_item_release(keys[field_index]);
        sxml_sequence_release(values[field_index]);
    }
    if (status == SXML_STATUS_OK) {
        *record = sxml_sequence_from_item(map);
    }
    return status;
}

/* Makes the plan map of a grouping. */
static sxml_status element_map_make_plan(const element_map_context *context,
                                         const element_map_grouping *grouping,
                                         sxml_sequence *plan) {
    element_map_buffer entries = {NULL, 0, 0};
    sxml_status status = SXML_STATUS_OK;
    const element_map_group *groups = grouping->groups.elements;
    for (size_t group_index = 0;
         status == SXML_STATUS_OK && group_index < grouping->groups.element_count;
         group_index += 1) {
        sxml_sequence record = sxml_sequence_empty();
        sxml_sequence key = sxml_sequence_empty();
        status = element_map_make_plan_record(context, &groups[group_index], &record);
        if (status != SXML_STATUS_OK || element_map_is_empty(record)) {
            continue;
        }
        status = element_map_make_plan_name(
            context,
            (element_map_plan_key_text){
                .name = groups[group_index].name,
                .prefix_text = groups[group_index].is_attribute
                                   ? SXML_BYTE_VIEW_FROM_LITERAL("@")
                                   : (sxml_byte_view){.bytes = NULL, .byte_count = 0}},
            &key);
        if (status == SXML_STATUS_OK) {
            status = element_map_buffer_reserve(context, &entries, sizeof(sxml_map_entry));
        }
        if (status == SXML_STATUS_OK) {
            ((sxml_map_entry *)entries.elements)[entries.element_count] =
                (sxml_map_entry){.key = key.content, .value = record};
            entries.element_count += 1;
        } else {
            sxml_sequence_release(key);
            sxml_sequence_release(record);
        }
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(context->item_heap,
                               &(sxml_map_request){.map_entries = {entries.elements,
                                                                   entries.element_count},
                                                   .duplicate_key_policy =
                                                       SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                                                   .error_record = NULL},
                               &map);
    }
    for (size_t entry_index = 0; entry_index < entries.element_count; entry_index += 1) {
        sxml_item_release(((sxml_map_entry *)entries.elements)[entry_index].key);
        sxml_sequence_release(((sxml_map_entry *)entries.elements)[entry_index].value);
    }
    element_map_buffer_release(context, &entries);
    if (status == SXML_STATUS_OK) {
        *plan = sxml_sequence_from_item(map);
    }
    return status;
}

/* fn:element-to-map-plan($input as (document-node() | element())*) as
   map(xs:string, record(...)) (F&O 4.0, section 14.6.13). */
static sxml_status call_element_to_map_plan(const sxml_function_call *call,
                                            sxml_sequence *result) {
    element_map_context context = element_map_start_context(call);
    element_map_conversion conversion = {.context = &context, .child_names = {NULL, 0, 0}};
    element_map_grouping grouping = {.groups = {NULL, 0, 0}, .index = {.representation = 0}};
    sxml_status status = sxml_map_make(
        context.item_heap,
        &(sxml_map_request){.map_entries = {NULL, 0},
                            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_LAST,
                            .error_record = NULL},
        &grouping.index);
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference node;
        sxml_node_description description;
        if (sxml_item_get_node(step.item, &node) != SXML_STATUS_OK) {
            status = element_map_report(&context, &element_map_type_error);
            break;
        }
        status = sxml_node_describe(node, &description);
        if (status == SXML_STATUS_OK && description.node_kind != SXML_NODE_KIND_ELEMENT &&
            description.node_kind != SXML_NODE_KIND_DOCUMENT) {
            status = element_map_report(&context, &element_map_type_error);
        }
        const uint32_t last_index = description.last_descendant.index;
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_charge_steps(
                call, (uint64_t)last_index - node.node_index.index + 1);
        }
        for (uint32_t node_index = node.node_index.index;
             status == SXML_STATUS_OK && node_index <= last_index; node_index += 1) {
            const sxml_node_reference current = {.tree = node.tree,
                                                 .node_index = {node_index},
                                                 .namespace_number = 0};
            sxml_node_description current_description;
            status = sxml_node_describe(current, &current_description);
            if (status == SXML_STATUS_OK &&
                current_description.node_kind == SXML_NODE_KIND_ELEMENT) {
                status = element_map_add_to_grouping(
                    &conversion,
                    (element_map_shape_request){.element = current,
                                                .description = &current_description},
                    &grouping);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_make_plan(&context, &grouping, result);
    }
    sxml_item_release(grouping.index);
    element_map_buffer_release(&context, &grouping.groups);
    element_map_buffer_release(&context, &conversion.child_names);
    element_map_context_release(&context);
    return status;
}

/* ------------------------------------------- Map-to-element: key roles */

/* The role of a map key (F&O 4.0, section 14.6.12). */
typedef enum element_map_role {
    SXML_ELEMENT_MAP_ROLE_CONTENT,
    SXML_ELEMENT_MAP_ROLE_COMMENT,
    SXML_ELEMENT_MAP_ROLE_PROCESSING_INSTRUCTION,
    SXML_ELEMENT_MAP_ROLE_ATTRIBUTE,
    SXML_ELEMENT_MAP_ROLE_ELEMENT,
} element_map_role;

/* What decides the roles of the keys of one map. */
typedef struct element_map_roles {
    sxml_item map;
    bool is_array_member;
    /* SIZE_MAX when the map has no content key. */
    size_t content_entry;
    sxml_byte_view attribute_marker;
    size_t entry_count;
} element_map_roles;

static bool element_map_starts_with(sxml_byte_view text, sxml_byte_view start) {
    return start.byte_count == 0 || (text.byte_count >= start.byte_count &&
                                     memcmp(text.bytes, start.bytes, start.byte_count) == 0);
}

/* True when text is the content key with zero or more "#" before it. */
static bool element_map_is_content_key_form(const element_map_context *context,
                                            sxml_byte_view text) {
    if (text.byte_count == 0) {
        return context->content_key.byte_count == 0;
    }
    size_t hash_count = 0;
    while (hash_count < text.byte_count && text.bytes[hash_count] == '#') {
        hash_count += 1;
    }
    for (size_t start = 0; start <= hash_count; start += 1) {
        if (sxml_byte_view_equals(
                (sxml_byte_view){.bytes = text.bytes + start, .byte_count = text.byte_count - start},
                context->content_key)) {
            return true;
        }
    }
    return false;
}

/* A map whose key roles are wanted. */
typedef struct element_map_roles_request {
    sxml_item map;
    bool is_array_member;
} element_map_roles_request;

static sxml_status element_map_describe_roles(const element_map_context *context,
                                              element_map_roles_request request,
                                              element_map_roles *roles) {
    const sxml_item map = request.map;
    const bool is_array_member = request.is_array_member;
    *roles = (element_map_roles){.map = map,
                                 .is_array_member = is_array_member,
                                 .content_entry = SIZE_MAX,
                                 .attribute_marker = context->attribute_marker,
                                 .entry_count = 0};
    sxml_status status = sxml_map_get_size(map, &roles->entry_count);
    size_t content_length = 0;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < roles->entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        sxml_byte_view text;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status != SXML_STATUS_OK || !element_map_item_text(entry.key, &text)) {
            continue;
        }
        const bool is_content =
            is_array_member ? sxml_byte_view_equals(text, context->content_key)
                            : element_map_is_content_key_form(context, text);
        if (is_content && (roles->content_entry == SIZE_MAX || text.byte_count > content_length)) {
            roles->content_entry = entry_index;
            content_length = text.byte_count;
        }
    }
    const bool marker_is_at = element_map_starts_with(context->attribute_marker,
                                                      SXML_BYTE_VIEW_FROM_LITERAL("@"));
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && !marker_is_at && entry_index < roles->entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        sxml_byte_view text;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status == SXML_STATUS_OK && entry_index != roles->content_entry &&
            element_map_item_text(entry.key, &text) && text.byte_count > 0 &&
            text.bytes[0] == '@') {
            /* The inverse of the conflict rule of F&O 4.0, section 14.6.8. */
            roles->attribute_marker = SXML_BYTE_VIEW_FROM_LITERAL("@");
        }
    }
    return status;
}

/* The role of one entry, and the name text left after the marker. */
typedef struct element_map_entry_role {
    element_map_role role;
    bool has_text_key;
    sxml_byte_view name_text;
    sxml_map_entry entry;
} element_map_entry_role;

static sxml_status element_map_role_of(const element_map_roles *roles, size_t entry_index,
                                       element_map_entry_role *role) {
    const sxml_status status = sxml_map_get_entry(roles->map, entry_index, &role->entry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    role->has_text_key = element_map_item_text(role->entry.key, &text);
    role->name_text = text;
    if (entry_index == roles->content_entry) {
        role->role = SXML_ELEMENT_MAP_ROLE_CONTENT;
    } else if (roles->is_array_member &&
               sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("#comment"))) {
        role->role = SXML_ELEMENT_MAP_ROLE_COMMENT;
    } else if (roles->is_array_member &&
               sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("#processing-instruction"))) {
        role->role = SXML_ELEMENT_MAP_ROLE_PROCESSING_INSTRUCTION;
    } else if (role->has_text_key && element_map_starts_with(text, roles->attribute_marker)) {
        role->role = SXML_ELEMENT_MAP_ROLE_ATTRIBUTE;
        role->name_text = text.byte_count == 0 ? text : (sxml_byte_view){
            .bytes = text.bytes + roles->attribute_marker.byte_count,
            .byte_count = text.byte_count - roles->attribute_marker.byte_count};
    } else {
        role->role = SXML_ELEMENT_MAP_ROLE_ELEMENT;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------ Map-to-element: decoding */

/* What a value is, for matching (F&O 4.0, section 14.6.12.1): the empty
   sequence counts as the zero-length string. */
typedef enum element_map_value_kind {
    SXML_ELEMENT_MAP_VALUE_KIND_NILLED,
    SXML_ELEMENT_MAP_VALUE_KIND_STRING,
    SXML_ELEMENT_MAP_VALUE_KIND_ATOMIC,
    SXML_ELEMENT_MAP_VALUE_KIND_MAP,
    SXML_ELEMENT_MAP_VALUE_KIND_ARRAY,
    SXML_ELEMENT_MAP_VALUE_KIND_OTHER,
} element_map_value_kind;

typedef struct element_map_value_class {
    element_map_value_kind value_kind;
    bool is_empty;
    bool is_zero_length;
    /* The item, unless is_empty. Borrowed. */
    sxml_item item;
} element_map_value_class;

static element_map_value_class element_map_classify(const element_map_context *context,
                                                    sxml_sequence value) {
    element_map_value_class value_class = {.value_kind = SXML_ELEMENT_MAP_VALUE_KIND_STRING,
                                           .is_empty = true,
                                           .is_zero_length = true,
                                           .item = {.representation = 0}};
    if (element_map_is_empty(value)) {
        return value_class;
    }
    value_class.is_empty = false;
    value_class.is_zero_length = false;
    value_class.value_kind = SXML_ELEMENT_MAP_VALUE_KIND_OTHER;
    if (!element_map_single_item(value, &value_class.item)) {
        return value_class;
    }
    const sxml_item_kind item_kind = sxml_item_get_kind(value_class.item);
    if (item_kind == SXML_ITEM_KIND_MAP) {
        value_class.value_kind = SXML_ELEMENT_MAP_VALUE_KIND_MAP;
    } else if (item_kind == SXML_ITEM_KIND_ARRAY) {
        value_class.value_kind = SXML_ELEMENT_MAP_VALUE_KIND_ARRAY;
    } else if (item_kind == SXML_ITEM_KIND_ATOMIC) {
        sxml_atomic_datum datum;
        sxml_name_parts parts;
        value_class.value_kind = SXML_ELEMENT_MAP_VALUE_KIND_ATOMIC;
        if (sxml_atomic_item_get_datum(value_class.item, &datum) != SXML_STATUS_OK) {
            return value_class;
        }
        if (datum.datum_kind == SXML_DATUM_KIND_STRING) {
            value_class.value_kind = SXML_ELEMENT_MAP_VALUE_KIND_STRING;
            value_class.is_zero_length = datum.datum_details.string.utf8.byte_count == 0;
        } else if (datum.datum_kind == SXML_DATUM_KIND_QNAME &&
                   sxml_item_get_type_annotation(value_class.item).index ==
                       (uint32_t)SXML_TYPE_XS_QNAME &&
                   sxml_name_pool_get_name_parts(context->name_pool,
                                                 datum.datum_details.qname.expanded_name,
                                                 &parts) == SXML_STATUS_OK &&
                   parts.namespace_uri.index == SXML_NAMESPACE_FUNCTIONS &&
                   sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("null"))) {
            /* The nilled marker #fn:null (F&O 4.0, section 14.6.12.2). */
            value_class.value_kind = SXML_ELEMENT_MAP_VALUE_KIND_NILLED;
        }
    }
    return value_class;
}

/* Appends the escaped form of name text (F&O 4.0, section 14.6.8): a
   character not allowed at its place in an NCName as _xHHHH_ (six digits
   above U+FFFF), "_" before "x" as _x005F_, the empty text as _x_. */
static sxml_status element_map_append_escaped(sxml_byte_view text, sxml_library_text *output) {
    if (text.byte_count == 0) {
        return sxml_function_library_text_append(output, SXML_BYTE_VIEW_FROM_LITERAL("_x_"));
    }
    sxml_status status = SXML_STATUS_OK;
    size_t position = 0;
    while (status == SXML_STATUS_OK && position < text.byte_count) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(text, position);
        const uint32_t codepoint = decoded.codepoint;
        const bool is_allowed =
            codepoint != ':' && (position == 0 ? sxml_xml_codepoint_is_name_start(codepoint)
                                               : sxml_xml_codepoint_is_name(codepoint));
        const bool is_escape_start = codepoint == '_' && position + 1 < text.byte_count &&
                                     text.bytes[position + 1] == 'x';
        if (is_allowed && !is_escape_start) {
            status = sxml_function_library_text_append(
                output, (sxml_byte_view){.bytes = text.bytes + position,
                                         .byte_count = decoded.encoded_byte_count});
        } else {
            static const char hex_digits[] = "0123456789ABCDEF";
            const size_t digit_count = codepoint > 0xFFFFU ? 6 : 4;
            uint8_t escape[10] = {'_', 'x'};
            for (size_t digit_index = 0; digit_index < digit_count; digit_index += 1) {
                escape[2 + digit_index] = (uint8_t)hex_digits[(codepoint >> (4U * (digit_count - 1 - digit_index))) & 0xFU];
            }
            escape[2 + digit_count] = '_';
            status = sxml_function_library_text_append(
                output, (sxml_byte_view){.bytes = escape, .byte_count = digit_count + 3});
        }
        position += decoded.encoded_byte_count;
    }
    return status;
}

/* A key to read as an element or attribute name. */
typedef struct element_map_decode_request {
    sxml_byte_view text;
    bool is_attribute;
    bool is_top;
    /* The parent element, for a local name in the default name format. */
    sxml_name_identifier parent_name;
    sxml_namespace_prefix_identifier parent_prefix;
} element_map_decode_request;

/* A name read from a key, with the prefix to write it with. */
typedef struct element_map_decoded_name {
    sxml_name_identifier name;
    sxml_namespace_prefix_identifier namespace_prefix;
} element_map_decoded_name;

/* The state of one call of fn:map-to-element. */
typedef struct element_map_building {
    element_map_context *context;
    sxml_tree_builder builder;
    /* The statically known namespaces of the call, borrowed. */
    sxml_namespace_binding_view static_namespaces;
    /* Namespace URIs given a made-up prefix ns0, ns1, ... */
    element_map_buffer prefixed_namespaces;
    /* The attribute names of the element being started. */
    element_map_buffer attribute_names;
    element_map_buffer frames;
} element_map_building;

/* The namespace a prefix is bound to: xml always, the static namespaces
   otherwise (not the default namespace); false when unbound. */
typedef struct element_map_resolved_prefix {
    sxml_namespace_prefix_identifier prefix;
    sxml_namespace_uri_identifier namespace_uri;
} element_map_resolved_prefix;

static bool element_map_resolve_prefix(const element_map_building *building,
                                       sxml_byte_view prefix_text,
                                       element_map_resolved_prefix *resolved) {
    sxml_namespace_prefix_identifier *prefix = &resolved->prefix;
    sxml_namespace_uri_identifier *namespace_uri = &resolved->namespace_uri;
    const element_map_context *context = building->context;
    if (sxml_name_pool_intern_namespace_prefix(context->name_pool, prefix_text, prefix) !=
        SXML_STATUS_OK) {
        return false;
    }
    if (sxml_byte_view_equals(prefix_text, SXML_BYTE_VIEW_FROM_LITERAL("xml"))) {
        *namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML);
        return true;
    }
    for (size_t binding_index = building->static_namespaces.namespace_binding_count;
         binding_index > 0; binding_index -= 1) {
        const sxml_namespace_binding *binding =
            &building->static_namespaces.namespace_bindings[binding_index - 1];
        if (binding->namespace_prefix.index == prefix->index && prefix->index != 0) {
            *namespace_uri = binding->namespace_uri;
            return binding->namespace_uri.index != SXML_NAMESPACE_NONE;
        }
    }
    return false;
}

/* A made-up prefix for an attribute in namespace_uri. */
static sxml_status element_map_make_prefix(element_map_building *building,
                                           sxml_namespace_uri_identifier namespace_uri,
                                           sxml_namespace_prefix_identifier *prefix) {
    const element_map_context *context = building->context;
    sxml_namespace_uri_identifier *uris = building->prefixed_namespaces.elements;
    size_t number = 0;
    while (number < building->prefixed_namespaces.element_count &&
           uris[number].index != namespace_uri.index) {
        number += 1;
    }
    sxml_status status = SXML_STATUS_OK;
    if (number == building->prefixed_namespaces.element_count) {
        status = element_map_buffer_reserve(context, &building->prefixed_namespaces,
                                            sizeof(sxml_namespace_uri_identifier));
        if (status != SXML_STATUS_OK) {
            return status;
        }
        ((sxml_namespace_uri_identifier *)building->prefixed_namespaces.elements)[number] =
            namespace_uri;
        building->prefixed_namespaces.element_count += 1;
    }
    uint8_t prefix_text[24] = {'n', 's'};
    size_t length = 2;
    uint8_t digits[20];
    size_t digit_count = 0;
    do {
        digits[digit_count] = (uint8_t)('0' + number % 10);
        digit_count += 1;
        number /= 10;
    } while (number > 0);
    while (digit_count > 0) {
        digit_count -= 1;
        prefix_text[length] = digits[digit_count];
        length += 1;
    }
    return sxml_name_pool_intern_namespace_prefix(
        context->name_pool, (sxml_byte_view){.bytes = prefix_text, .byte_count = length}, prefix);
}

/* Reads a key as a name (F&O 4.0, section 14.6.12, the inverse of
   14.6.8); FOJS0009 for a key that gives no valid name. */
static sxml_status element_map_decode_name(element_map_building *building,
                                           element_map_decode_request request,
                                           element_map_decoded_name *decoded) {
    const element_map_context *context = building->context;
    sxml_library_text local = {.item_heap = context->item_heap};
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    bool is_valid = true;
    bool has_namespace = false;
    bool is_eqname = false;
    sxml_byte_view local_text = request.text;
    sxml_status status = SXML_STATUS_OK;
    element_map_eqname eqname;
    if (element_map_split_eqname(request.text, &eqname)) {
        is_eqname = true;
        has_namespace = true;
        local_text = eqname.local_name;
        if (eqname.namespace_uri.byte_count > 0) {
            status = sxml_name_pool_intern_namespace_uri(context->name_pool, eqname.namespace_uri,
                                                         &namespace_uri);
        }
    } else {
        const uint8_t *colon = request.text.byte_count == 0
                                   ? NULL
                                   : memchr(request.text.bytes, ':', request.text.byte_count);
        const bool splits =
            colon != NULL &&
            (!context->escapes_names ||
             context->name_format == SXML_ELEMENT_MAP_NAME_FORMAT_LEXICAL ||
             element_map_starts_with(request.text, SXML_BYTE_VIEW_FROM_LITERAL("xml:")));
        if (splits) {
            const sxml_byte_view prefix_text = {
                .bytes = request.text.bytes, .byte_count = (size_t)(colon - request.text.bytes)};
            local_text = (sxml_byte_view){.bytes = colon + 1,
                                          .byte_count = request.text.byte_count -
                                                        prefix_text.byte_count - 1};
            has_namespace = true;
            element_map_resolved_prefix resolved = {.prefix = {0}, .namespace_uri = {0}};
            is_valid = sxml_xml_text_is_ncname(prefix_text) &&
                       element_map_resolve_prefix(building, prefix_text, &resolved);
            prefix = resolved.prefix;
            namespace_uri = resolved.namespace_uri;
        }
    }
    if (status == SXML_STATUS_OK && is_valid) {
        if (context->escapes_names) {
            status = element_map_append_escaped(local_text, &local);
        } else {
            is_valid = sxml_xml_text_is_ncname(local_text);
            status = sxml_function_library_text_append(&local, local_text);
        }
    }
    if (status == SXML_STATUS_OK && is_valid && !has_namespace && !request.is_attribute &&
        !request.is_top && context->name_format == SXML_ELEMENT_MAP_NAME_FORMAT_DEFAULT) {
        /* A local name in the default format: the parent's namespace. */
        namespace_uri = element_map_namespace_of(context, request.parent_name);
        prefix = request.parent_prefix;
    }
    if (status == SXML_STATUS_OK && is_valid) {
        is_valid = namespace_uri.index != SXML_NAMESPACE_XMLNS &&
                   sxml_function_library_text_view(&local).byte_count > 0 &&
                   !(request.is_attribute && namespace_uri.index == SXML_NAMESPACE_NONE &&
                     sxml_byte_view_equals(sxml_function_library_text_view(&local),
                                           SXML_BYTE_VIEW_FROM_LITERAL("xmlns")));
    }
    if (status == SXML_STATUS_OK && is_valid && is_eqname) {
        if (namespace_uri.index == SXML_NAMESPACE_XML) {
            status = sxml_name_pool_intern_namespace_prefix(
                context->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xml"), &prefix);
        } else if (request.is_attribute && namespace_uri.index != SXML_NAMESPACE_NONE) {
            status = element_map_make_prefix(building, namespace_uri, &prefix);
        }
    }
    if (status == SXML_STATUS_OK && is_valid) {
        status = sxml_name_pool_intern_name(
            context->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = sxml_function_library_text_view(&local)},
            &decoded->name);
        decoded->namespace_prefix = prefix;
    }
    sxml_function_library_text_release(&local);
    if (status == SXML_STATUS_OK && !is_valid) {
        return element_map_report(context, &element_map_form_error);
    }
    return status;
}

/* ------------------------------------------ Map-to-element: matching */

/* The candidate layouts of an element name (F&O 4.0, 14.6.12.1). */
typedef struct element_map_candidates {
    element_map_layout layouts[4];
    const element_map_plan_entry *entries[4];
    size_t candidate_count;
} element_map_candidates;

static sxml_status element_map_find_candidates(const element_map_context *context,
                                               sxml_name_identifier name,
                                               element_map_candidates *candidates) {
    candidates->candidate_count = 0;
    const element_map_plan_entry *entry = NULL;
    const sxml_status status = element_map_find_plan_entry(
        context, (element_map_name_query){.name = name, .is_attribute = false}, &entry);
    const element_map_plan_entry *choices[2] = {entry,
                                                context->has_fallback ? &context->fallback : NULL};
    for (size_t choice_index = 0; choice_index < 2; choice_index += 1) {
        const element_map_plan_entry *choice = choices[choice_index];
        if (choice != NULL && choice->layout != SXML_ELEMENT_MAP_LAYOUT_DEEP_SKIP &&
            choice->layout != SXML_ELEMENT_MAP_LAYOUT_ERROR) {
            candidates->layouts[candidates->candidate_count] = choice->layout;
            candidates->entries[candidates->candidate_count] = choice;
            candidates->candidate_count += 1;
        }
    }
    if (candidates->candidate_count == 0) {
        static const element_map_layout defaults[] = {
            SXML_ELEMENT_MAP_LAYOUT_SIMPLE, SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS,
            SXML_ELEMENT_MAP_LAYOUT_RECORD, SXML_ELEMENT_MAP_LAYOUT_MIXED};
        for (size_t default_index = 0; default_index < 4; default_index += 1) {
            candidates->layouts[default_index] = defaults[default_index];
            candidates->entries[default_index] = NULL;
        }
        candidates->candidate_count = 4;
    }
    return status;
}

/* A value to match against a layout. */
typedef struct element_map_match_request {
    sxml_sequence value;
    element_map_layout layout;
    const element_map_plan_entry *entry;
    /* The element the value belongs to, for the names of its keys. */
    sxml_name_identifier name;
} element_map_match_request;

/* What the keys of an element's value map are. */
typedef struct element_map_map_facts {
    size_t element_key_count;
    size_t element_entry;
    bool has_content;
    element_map_value_class content;
} element_map_map_facts;

static sxml_status element_map_describe_map(const element_map_context *context, sxml_item map,
                                            element_map_map_facts *facts) {
    element_map_roles roles;
    sxml_status status = element_map_describe_roles(
            context, (element_map_roles_request){.map = map, .is_array_member = false},
            &roles);
    *facts = (element_map_map_facts){.element_key_count = 0, .has_content = false};
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < roles.entry_count;
         entry_index += 1) {
        element_map_entry_role role;
        status = element_map_role_of(&roles, entry_index, &role);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (role.role == SXML_ELEMENT_MAP_ROLE_CONTENT) {
            facts->has_content = true;
            facts->content = element_map_classify(context, role.entry.value);
        } else if (role.role == SXML_ELEMENT_MAP_ROLE_ELEMENT) {
            facts->element_key_count += 1;
            facts->element_entry = entry_index;
        }
    }
    return status;
}

/* True for the member of a mixed or sequence array: a single-entry map,
   or (mixed only) a string or the empty sequence. */
static bool element_map_is_mixed_member(const element_map_context *context, sxml_sequence member,
                                        element_map_layout layout) {
    const bool allows_strings = layout == SXML_ELEMENT_MAP_LAYOUT_MIXED;
    const element_map_value_class value_class = element_map_classify(context, member);
    if (value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_STRING) {
        return allows_strings;
    }
    size_t entry_count = 0;
    return value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_MAP &&
           sxml_map_get_size(value_class.item, &entry_count) == SXML_STATUS_OK &&
           entry_count == 1;
}

/* Whether a value has the form of a layout (F&O 4.0, section 14.6.12.2,
   the column "Matching values"); for the list layout only that it is an
   array (its members are checked by element_map_value_matches). */
static sxml_status element_map_value_has_form(element_map_building *building,
                                             element_map_match_request request, element_map_answer *matches) {
    const element_map_context *context = building->context;
    const element_map_value_class value_class = element_map_classify(context, request.value);
    matches->is_true = false;
    if (value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_NILLED) {
        matches->is_true = true;
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    switch (request.layout) {
    case SXML_ELEMENT_MAP_LAYOUT_EMPTY:
        matches->is_true = value_class.is_zero_length;
        break;
    case SXML_ELEMENT_MAP_LAYOUT_SIMPLE:
        matches->is_true = value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_STRING ||
                   value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_ATOMIC;
        break;
    case SXML_ELEMENT_MAP_LAYOUT_XML:
        matches->is_true = value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_STRING &&
                   !value_class.is_empty;
        break;
    case SXML_ELEMENT_MAP_LAYOUT_EMPTY_PLUS:
    case SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS:
    case SXML_ELEMENT_MAP_LAYOUT_RECORD:
    case SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS: {
        if (value_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_MAP) {
            break;
        }
        element_map_map_facts facts;
        status = element_map_describe_map(context, value_class.item, &facts);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const bool content_is_nilled =
            !facts.has_content || facts.content.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_NILLED;
        if (request.layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS) {
            matches->is_true = facts.has_content && facts.element_key_count == 0 &&
                       (facts.content.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_STRING ||
                        facts.content.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_ATOMIC ||
                        facts.content.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_NILLED);
        } else if (request.layout == SXML_ELEMENT_MAP_LAYOUT_EMPTY_PLUS) {
            matches->is_true = content_is_nilled && facts.element_key_count == 0;
        } else if (request.layout == SXML_ELEMENT_MAP_LAYOUT_RECORD) {
            matches->is_true = content_is_nilled;
        } else {
            matches->is_true = content_is_nilled && facts.element_key_count <= 1;
            if (matches->is_true && facts.element_key_count == 1) {
                element_map_roles roles;
                element_map_entry_role role;
                status = element_map_describe_roles(
            context, (element_map_roles_request){.map = value_class.item, .is_array_member = false},
            &roles);
                if (status == SXML_STATUS_OK) {
                    status = element_map_role_of(&roles, facts.element_entry, &role);
                }
                element_map_decoded_name decoded = {.name = {.index = 0}};
                const element_map_value_class member_class =
                    element_map_classify(context, role.entry.value);
                matches->is_true = status == SXML_STATUS_OK && role.has_text_key &&
                           member_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_ARRAY;
                if (matches->is_true) {
                    status = element_map_decode_name(
                        building,
                        (element_map_decode_request){.text = role.name_text,
                                                     .parent_name = request.name},
                        &decoded);
                    matches->is_true = status == SXML_STATUS_OK && request.entry != NULL &&
                               decoded.name.index == request.entry->child_name.index;
                    if (status == SXML_STATUS_DYNAMIC_ERROR) {
                        status = SXML_STATUS_OK;
                    }
                }
            }
        }
        break;
    }
    case SXML_ELEMENT_MAP_LAYOUT_LIST:
    case SXML_ELEMENT_MAP_LAYOUT_MIXED:
    case SXML_ELEMENT_MAP_LAYOUT_SEQUENCE: {
        size_t member_count = 0;
        if (value_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_ARRAY ||
            sxml_array_get_size(value_class.item, &member_count) != SXML_STATUS_OK) {
            break;
        }
        matches->is_true = true;
        for (size_t member_index = 0;
             status == SXML_STATUS_OK && matches->is_true && member_index < member_count;
             member_index += 1) {
            sxml_sequence member = sxml_sequence_empty();
            status = sxml_array_get_member(value_class.item, member_index, &member);
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (request.layout == SXML_ELEMENT_MAP_LAYOUT_LIST) {
                break;
            } else {
                matches->is_true = element_map_is_mixed_member(
                    context, member, request.layout);
            }
        }
        break;
    }
    case SXML_ELEMENT_MAP_LAYOUT_NONE:
    case SXML_ELEMENT_MAP_LAYOUT_ERROR:
    case SXML_ELEMENT_MAP_LAYOUT_DEEP_SKIP:
        break;
    }
    return status;
}

/* Whether a value has the form of one of the candidate layouts of
   request.name. */
static sxml_status element_map_has_candidate_form(element_map_building *building,
                                                  element_map_match_request request,
                                                  element_map_answer *matches) {
    element_map_candidates candidates;
    sxml_status status = element_map_find_candidates(building->context, request.name, &candidates);
    matches->is_true = false;
    for (size_t candidate_index = 0;
         status == SXML_STATUS_OK && !matches->is_true && candidate_index < candidates.candidate_count;
         candidate_index += 1) {
        request.layout = candidates.layouts[candidate_index];
        request.entry = candidates.entries[candidate_index];
        status = element_map_value_has_form(building, request, matches);
    }
    return status;
}

/* Whether a value matches a layout: its form, and for the list layout
   the form of each member for the plan's child name (one level, so no
   recursion, D71). */
static sxml_status element_map_value_matches(element_map_building *building,
                                             element_map_match_request request, element_map_answer *matches) {
    sxml_status status = element_map_value_has_form(building, request, matches);
    if (status != SXML_STATUS_OK || !matches->is_true || request.layout != SXML_ELEMENT_MAP_LAYOUT_LIST) {
        return status;
    }
    const element_map_value_class value_class = element_map_classify(building->context,
                                                                      request.value);
    size_t member_count = 0;
    if (value_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_ARRAY) {
        return status;
    }
    status = sxml_array_get_size(value_class.item, &member_count);
    for (size_t member_index = 0;
         status == SXML_STATUS_OK && matches->is_true && member_index < member_count; member_index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(value_class.item, member_index, &member);
        if (status == SXML_STATUS_OK && request.entry != NULL) {
            status = element_map_has_candidate_form(
                building,
                (element_map_match_request){.value = member, .name = request.entry->child_name},
                matches);
        }
    }
    return status;
}

/* The first candidate layout of request.name that the value matches. */
typedef struct element_map_selection {
    bool matches;
    element_map_layout layout;
    const element_map_plan_entry *entry;
} element_map_selection;

static sxml_status element_map_select_candidate(element_map_building *building,
                                                element_map_match_request request,
                                                element_map_selection *selection) {
    element_map_candidates candidates;
    sxml_status status = element_map_find_candidates(building->context, request.name, &candidates);
    selection->matches = false;
    for (size_t candidate_index = 0;
         status == SXML_STATUS_OK && !selection->matches &&
         candidate_index < candidates.candidate_count;
         candidate_index += 1) {
        request.layout = candidates.layouts[candidate_index];
        request.entry = candidates.entries[candidate_index];
        element_map_answer matches = {.is_true = false};
        status = element_map_value_matches(building, request, &matches);
        selection->matches = matches.is_true;
        selection->layout = request.layout;
        selection->entry = request.entry;
    }
    return status;
}

/* ------------------------------------------ Map-to-element: building */

/* An element being built: one entry of the explicit stack (D71). */
typedef struct element_map_frame {
    /* Borrowed from the map argument. */
    sxml_sequence value;
    sxml_name_identifier name;
    sxml_namespace_prefix_identifier namespace_prefix;
    bool is_top;
    bool is_started;
    bool has_previous_string;
    element_map_layout layout;
    const element_map_plan_entry *entry;
    /* The next entry or member to look at. */
    size_t position;
    /* For an element key whose array gives one child per member: the
       name, the array and the next member. */
    bool is_spreading;
    sxml_item spread_array;
    size_t spread_member;
    size_t spread_count;
    element_map_decoded_name spread_name;
} element_map_frame;

/* The lexical form of an atomic item (its cast to xs:string). */
static sxml_status element_map_append_lexical(const element_map_context *context,
                                              sxml_item item, sxml_library_text *text) {
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_canonical_text canonical = {.text = {.bytes = NULL, .byte_count = 0},
                                     .required_byte_count = 0};
    sxml_status status = sxml_cast_write_canonical_text(
        context->item_heap,
        &(sxml_canonical_text_request){.item = item,
                                       .output = {.bytes = output, .byte_count = sizeof(output)}},
        &canonical);
    if (status == SXML_STATUS_CAPACITY_EXCEEDED) {
        void *memory = NULL;
        status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = canonical.required_byte_count, .alignment = 1},
            &memory);
        if (status == SXML_STATUS_OK) {
            status = sxml_cast_write_canonical_text(
                context->item_heap,
                &(sxml_canonical_text_request){
                    .item = item,
                    .output = {.bytes = memory, .byte_count = canonical.required_byte_count}},
                &canonical);
            if (status == SXML_STATUS_OK) {
                status = sxml_function_library_text_append(text, canonical.text);
            }
            sxml_allocator_release(context->allocator, memory);
        }
        return status;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(text, canonical.text);
    }
    return status;
}

/* The text a value contributes as attribute value or text: one atomic
   item (not the nilled marker) or the empty sequence; FOJS0009 else. */
static sxml_status element_map_value_text(const element_map_context *context,
                                          sxml_sequence value, sxml_library_text *text) {
    const element_map_value_class value_class = element_map_classify(context, value);
    if (value_class.is_empty) {
        return SXML_STATUS_OK;
    }
    if (value_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_STRING &&
        value_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_ATOMIC) {
        return element_map_report(context, &element_map_form_error);
    }
    return element_map_append_lexical(context, value_class.item, text);
}

static sxml_status element_map_send_text(element_map_building *building, sxml_sequence value) {
    sxml_library_text text = {.item_heap = building->context->item_heap};
    sxml_status status = element_map_value_text(building->context, value, &text);
    if (status == SXML_STATUS_OK && text.byte_count > 0) {
        status = building->builder.operations->add_text(
            building->builder.state,
            &(sxml_text_event){.text = sxml_function_library_text_view(&text),
                               .location = {.line_number = 0}});
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* An attribute to add to the element being started. */
typedef struct element_map_attribute {
    element_map_decoded_name name;
    sxml_byte_view value;
} element_map_attribute;

static sxml_status element_map_send_attribute(element_map_building *building,
                                              element_map_attribute attribute) {
    const element_map_context *context = building->context;
    const uint32_t *names = building->attribute_names.elements;
    for (size_t name_index = 0; name_index < building->attribute_names.element_count;
         name_index += 1) {
        if (names[name_index] == attribute.name.name.index) {
            /* Two attributes of one name: not well-formed. */
            return element_map_report(context, &element_map_form_error);
        }
    }
    sxml_status status =
        element_map_buffer_reserve(context, &building->attribute_names, sizeof(uint32_t));
    if (status == SXML_STATUS_OK) {
        ((uint32_t *)building->attribute_names.elements)[building->attribute_names.element_count] =
            attribute.name.name.index;
        building->attribute_names.element_count += 1;
        status = building->builder.operations->add_attribute(
            building->builder.state,
            &(sxml_attribute_event){.attribute_name = attribute.name.name,
                                    .namespace_prefix = attribute.name.namespace_prefix,
                                    .attribute_value = attribute.value,
                                    .type_annotation = {.index = SXML_TYPE_NONE},
                                    .location = {.line_number = 0}});
    }
    return status;
}

/* xsi:nil="true" for a nilled marker (F&O 4.0, section 14.6.12.2). */
static sxml_status element_map_send_nilled(element_map_building *building) {
    const element_map_context *context = building->context;
    element_map_decoded_name name;
    sxml_status status = sxml_name_pool_intern_name(
        context->name_pool,
        &(sxml_name_request){
            .namespace_uri =
                sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML_SCHEMA_INSTANCE),
            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("nil")},
        &name.name);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            context->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xsi"), &name.namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_send_attribute(
            building, (element_map_attribute){.name = name,
                                              .value = SXML_BYTE_VIEW_FROM_LITERAL("true")});
    }
    return status;
}

/* An attribute key with its value. */
static sxml_status element_map_send_attribute_entry(element_map_building *building,
                                                    const element_map_frame *frame,
                                                    const element_map_entry_role *role) {
    const element_map_context *context = building->context;
    if (element_map_classify(context, role->entry.value).value_kind ==
        SXML_ELEMENT_MAP_VALUE_KIND_NILLED) {
        return element_map_report(context, &element_map_form_error);
    }
    element_map_decoded_name name;
    sxml_status status = element_map_decode_name(
        building,
        (element_map_decode_request){.text = role->name_text,
                                     .is_attribute = true,
                                     .parent_name = frame->name,
                                     .parent_prefix = frame->namespace_prefix},
        &name);
    sxml_library_text text = {.item_heap = context->item_heap};
    if (status == SXML_STATUS_OK) {
        status = element_map_value_text(context, role->entry.value, &text);
    }
    if (status == SXML_STATUS_OK) {
        status = element_map_send_attribute(
            building, (element_map_attribute){.name = name,
                                              .value = sxml_function_library_text_view(&text)});
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* Pushes a frame for a child element. */
static sxml_status element_map_push_frame(element_map_building *building,
                                          const element_map_frame *frame) {
    sxml_status status =
        element_map_buffer_reserve(building->context, &building->frames, sizeof(element_map_frame));
    if (status == SXML_STATUS_OK) {
        ((element_map_frame *)building->frames.elements)[building->frames.element_count] = *frame;
        building->frames.element_count += 1;
        /* Each element is one step (D89). */
        status = sxml_function_library_charge_steps(building->context->call, 1);
    }
    return status;
}

/* The child frame for a value named by a decoded name. */
static element_map_frame element_map_child_frame(element_map_decoded_name name,
                                                 sxml_sequence value) {
    element_map_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.value = value;
    frame.name = name.name;
    frame.namespace_prefix = name.namespace_prefix;
    return frame;
}

/* The xml layout: the string parsed as by fn:parse-xml, its one element
   copied (F&O 4.0, section 14.6.12.2). */
static sxml_status element_map_send_xml(element_map_building *building, sxml_sequence value) {
    const element_map_context *context = building->context;
    const sxml_function_call *call = context->call;
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    (void)element_map_item_text(value.content, &text);
    sxml_item document = {.representation = 0};
    sxml_status status = sxml_dynamic_context_parse_document(
        call->dynamic_context,
        &(sxml_document_parse_request){.xml_bytes = text,
                                       .base_uri = {.bytes = NULL, .byte_count = 0},
                                       .error_record = call->error_record},
        &document);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return element_map_report(context, &element_map_form_error);
    }
    sxml_node_reference node = {.tree = NULL, .node_index = {0}, .namespace_number = 0};
    sxml_node_description description = {.has_children = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(document, &node);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    size_t child_count = 0;
    sxml_node_reference element = node;
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_reference child = {.tree = node.tree, .node_index = description.first_child,
                                 .namespace_number = 0};
    bool has_element = false;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = sxml_node_describe(child, &child_description);
        child_count += 1;
        if (status == SXML_STATUS_OK && child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            element = child;
            has_element = true;
        }
        has_child = status == SXML_STATUS_OK && child_description.has_next_sibling;
        child.node_index = child_description.next_sibling;
    }
    if (status == SXML_STATUS_OK && (child_count != 1 || !has_element)) {
        status = element_map_report(context, &element_map_form_error);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_builder_copy_node(
            building->builder, &(sxml_copy_request){.node = element,
                                                    .preserves_namespaces = true,
                                                    .inherits_namespaces = true,
                                                    .preserves_type_annotations = false});
    }
    sxml_item_release(document);
    return status;
}

/* Starts the element of the top frame: chooses its layout, sends the
   start event and the attributes; an element without children is ended
   too (*is_finished). */
static sxml_status element_map_start_frame(element_map_building *building, element_map_answer *is_finished) {
    const element_map_context *context = building->context;
    element_map_frame *frame =
        &((element_map_frame *)building->frames.elements)[building->frames.element_count - 1];
    element_map_selection selection = {.matches = false};
    sxml_status status = element_map_select_candidate(
        building, (element_map_match_request){.value = frame->value, .name = frame->name},
        &selection);
    if (status == SXML_STATUS_OK && !selection.matches) {
        return element_map_report(context, &element_map_form_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->layout = selection.layout;
    frame->entry = selection.entry;
    frame->is_started = true;
    is_finished->is_true = true;
    const element_map_value_class value_class = element_map_classify(context, frame->value);
    if (value_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_NILLED &&
        frame->layout == SXML_ELEMENT_MAP_LAYOUT_XML) {
        return element_map_send_xml(building, frame->value);
    }
    building->attribute_names.element_count = 0;
    status = building->builder.operations->start_element(
        building->builder.state,
        &(sxml_element_start_event){.element_name = frame->name,
                                    .namespace_prefix = frame->namespace_prefix,
                                    .type_annotation = {.index = SXML_TYPE_NONE},
                                    .location = {.line_number = 0}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_NILLED) {
        status = element_map_send_nilled(building);
    } else if (frame->layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE) {
        status = element_map_send_text(building, frame->value);
    } else if (value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_MAP) {
        element_map_roles roles;
        status = element_map_describe_roles(
            context, (element_map_roles_request){.map = value_class.item, .is_array_member = false},
            &roles);
        for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < roles.entry_count;
             entry_index += 1) {
            element_map_entry_role role;
            status = element_map_role_of(&roles, entry_index, &role);
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (!role.has_text_key) {
                status = element_map_report(context, &element_map_form_error);
            } else if (role.role == SXML_ELEMENT_MAP_ROLE_ATTRIBUTE) {
                status = element_map_send_attribute_entry(building, frame, &role);
            } else if (role.role == SXML_ELEMENT_MAP_ROLE_CONTENT &&
                       element_map_classify(context, role.entry.value).value_kind ==
                           SXML_ELEMENT_MAP_VALUE_KIND_NILLED) {
                status = element_map_send_nilled(building);
            }
        }
        is_finished->is_true = frame->layout == SXML_ELEMENT_MAP_LAYOUT_EMPTY_PLUS;
        if (status == SXML_STATUS_OK && frame->layout == SXML_ELEMENT_MAP_LAYOUT_SIMPLE_PLUS &&
            roles.content_entry != SIZE_MAX) {
            element_map_entry_role role;
            status = element_map_role_of(&roles, roles.content_entry, &role);
            if (status == SXML_STATUS_OK &&
                element_map_classify(context, role.entry.value).value_kind !=
                    SXML_ELEMENT_MAP_VALUE_KIND_NILLED) {
                status = element_map_send_text(building, role.entry.value);
            }
            is_finished->is_true = true;
        }
    } else if (value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_ARRAY) {
        is_finished->is_true = false;
        if (frame->layout == SXML_ELEMENT_MAP_LAYOUT_MIXED ||
            frame->layout == SXML_ELEMENT_MAP_LAYOUT_SEQUENCE) {
            /* Leading members with an attribute or the nilled marker. */
            size_t member_count = 0;
            bool has_nilled_member = false;
            status = sxml_array_get_size(value_class.item, &member_count);
            while (status == SXML_STATUS_OK && frame->position < member_count) {
                sxml_sequence member = sxml_sequence_empty();
                status = sxml_array_get_member(value_class.item, frame->position, &member);
                const element_map_value_class member_class =
                    element_map_classify(context, member);
                if (status != SXML_STATUS_OK ||
                    member_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_MAP) {
                    break;
                }
                element_map_roles roles;
                element_map_entry_role role;
                status = element_map_describe_roles(
            context, (element_map_roles_request){.map = member_class.item, .is_array_member = true},
            &roles);
                if (status == SXML_STATUS_OK) {
                    status = element_map_role_of(&roles, 0, &role);
                }
                if (status != SXML_STATUS_OK || (role.role != SXML_ELEMENT_MAP_ROLE_ATTRIBUTE &&
                                                 role.role != SXML_ELEMENT_MAP_ROLE_CONTENT)) {
                    break;
                }
                const bool is_nilled_marker =
                    role.role == SXML_ELEMENT_MAP_ROLE_CONTENT &&
                    element_map_classify(context, role.entry.value).value_kind ==
                        SXML_ELEMENT_MAP_VALUE_KIND_NILLED;
                if ((role.role == SXML_ELEMENT_MAP_ROLE_ATTRIBUTE && has_nilled_member) ||
                    (role.role == SXML_ELEMENT_MAP_ROLE_CONTENT && !is_nilled_marker)) {
                    /* An attribute after the nilled marker, or content. */
                    status = element_map_report(context, &element_map_form_error);
                } else if (role.role == SXML_ELEMENT_MAP_ROLE_ATTRIBUTE) {
                    status = element_map_send_attribute_entry(building, frame, &role);
                } else {
                    status = element_map_send_nilled(building);
                    has_nilled_member = true;
                }
                frame->position += 1;
            }
        }
    }
    if (status == SXML_STATUS_OK && is_finished->is_true) {
        status = building->builder.operations->end_element(building->builder.state);
    }
    return status;
}

/* The value of the entry of map whose key is the string text; empty
   when there is none. */
static sxml_status element_map_find_text_entry(sxml_item map, sxml_byte_view text,
                                               sxml_sequence *value) {
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    *value = sxml_sequence_empty();
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        sxml_byte_view key_text;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status == SXML_STATUS_OK && element_map_item_text(entry.key, &key_text) &&
            sxml_byte_view_equals(key_text, text)) {
            *value = entry.value;
        }
    }
    return status;
}

static bool element_map_contains(sxml_byte_view text, sxml_byte_view part) {
    for (size_t start = 0; start + part.byte_count <= text.byte_count; start += 1) {
        if (memcmp(text.bytes + start, part.bytes, part.byte_count) == 0) {
            return true;
        }
    }
    return false;
}

/* A comment or processing instruction of a mixed array, checked for
   well-formedness (XML 1.0, productions 15 and 16). */
static sxml_status element_map_send_node(element_map_building *building,
                                         const element_map_entry_role *role) {
    const element_map_context *context = building->context;
    sxml_library_text text = {.item_heap = context->item_heap};
    sxml_library_text target = {.item_heap = context->item_heap};
    sxml_status status = SXML_STATUS_OK;
    bool is_valid = true;
    if (role->role == SXML_ELEMENT_MAP_ROLE_COMMENT) {
        status = element_map_value_text(context, role->entry.value, &text);
        const sxml_byte_view comment = sxml_function_library_text_view(&text);
        is_valid = !element_map_contains(comment, SXML_BYTE_VIEW_FROM_LITERAL("--")) &&
                   (comment.byte_count == 0 || comment.bytes[comment.byte_count - 1] != '-');
        if (status == SXML_STATUS_OK && is_valid) {
            status = building->builder.operations->add_comment(
                building->builder.state,
                &(sxml_comment_event){.comment = comment, .location = {.line_number = 0}});
        }
    } else {
        const element_map_value_class value_class =
            element_map_classify(context, role->entry.value);
        sxml_sequence target_value = sxml_sequence_empty();
        sxml_sequence data_value = sxml_sequence_empty();
        size_t entry_count = 0;
        is_valid = value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_MAP &&
                   sxml_map_get_size(value_class.item, &entry_count) == SXML_STATUS_OK &&
                   entry_count == 2;
        if (is_valid) {
            status = element_map_find_text_entry(
                value_class.item, SXML_BYTE_VIEW_FROM_LITERAL("#target"), &target_value);
        }
        if (is_valid && status == SXML_STATUS_OK) {
            status = element_map_find_text_entry(
                value_class.item, SXML_BYTE_VIEW_FROM_LITERAL("#data"), &data_value);
        }
        if (is_valid && status == SXML_STATUS_OK) {
            status = element_map_value_text(context, target_value, &target);
        }
        if (is_valid && status == SXML_STATUS_OK) {
            status = element_map_value_text(context, data_value, &text);
        }
        const sxml_byte_view target_text = sxml_function_library_text_view(&target);
        const sxml_byte_view data = sxml_function_library_text_view(&text);
        is_valid = is_valid && sxml_xml_text_is_ncname(target_text) &&
                   !(target_text.byte_count == 3 && (target_text.bytes[0] | 0x20) == 'x' &&
                     (target_text.bytes[1] | 0x20) == 'm' && (target_text.bytes[2] | 0x20) == 'l') &&
                   !element_map_contains(data, SXML_BYTE_VIEW_FROM_LITERAL("?>"));
        if (status == SXML_STATUS_OK && is_valid) {
            status = building->builder.operations->add_processing_instruction(
                building->builder.state,
                &(sxml_processing_instruction_event){
                    .target = target_text, .data = data, .location = {.line_number = 0}});
        }
    }
    sxml_function_library_text_release(&text);
    sxml_function_library_text_release(&target);
    if (status == SXML_STATUS_OK && !is_valid) {
        return element_map_report(context, &element_map_form_error);
    }
    return status;
}

/* Whether every member of a non-empty array matches a name (the record
   layout's test for repeated children, F&O 4.0, section 14.6.12.2). */
static sxml_status element_map_members_match(element_map_building *building,
                                             element_map_match_request request, element_map_answer *matches) {
    const element_map_value_class value_class = element_map_classify(building->context,
                                                                      request.value);
    size_t member_count = 0;
    matches->is_true = false;
    if (value_class.value_kind != SXML_ELEMENT_MAP_VALUE_KIND_ARRAY ||
        sxml_array_get_size(value_class.item, &member_count) != SXML_STATUS_OK ||
        member_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    matches->is_true = true;
    for (size_t member_index = 0;
         status == SXML_STATUS_OK && matches->is_true && member_index < member_count; member_index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(value_class.item, member_index, &member);
        element_map_selection selection = {.matches = false};
        if (status == SXML_STATUS_OK) {
            status = element_map_select_candidate(
                building, (element_map_match_request){.value = member, .name = request.name},
                &selection);
        }
        matches->is_true = selection.matches;
    }
    return status;
}

/* Takes the next step of the top frame, whose element is started: pushes
   a child frame, sends text or a node, or ends the element
   (*is_finished). */
static sxml_status element_map_continue_frame(element_map_building *building,
                                              element_map_answer *is_finished) {
    const element_map_context *context = building->context;
    element_map_frame *frame =
        &((element_map_frame *)building->frames.elements)[building->frames.element_count - 1];
    const element_map_value_class value_class = element_map_classify(context, frame->value);
    is_finished->is_true = false;
    sxml_status status = SXML_STATUS_OK;
    if (frame->is_spreading) {
        if (frame->spread_member < frame->spread_count) {
            sxml_sequence member = sxml_sequence_empty();
            status = sxml_array_get_member(frame->spread_array, frame->spread_member, &member);
            frame->spread_member += 1;
            const element_map_frame child = element_map_child_frame(frame->spread_name, member);
            return status == SXML_STATUS_OK ? element_map_push_frame(building, &child) : status;
        }
        frame->is_spreading = false;
    }
    if (value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_MAP) {
        element_map_roles roles;
        status = element_map_describe_roles(
            context, (element_map_roles_request){.map = value_class.item, .is_array_member = false},
            &roles);
        while (status == SXML_STATUS_OK && frame->position < roles.entry_count) {
            element_map_entry_role role;
            status = element_map_role_of(&roles, frame->position, &role);
            frame->position += 1;
            if (status != SXML_STATUS_OK || role.role != SXML_ELEMENT_MAP_ROLE_ELEMENT) {
                continue;
            }
            element_map_decoded_name name;
            status = element_map_decode_name(
                building,
                (element_map_decode_request){.text = role.name_text,
                                             .parent_name = frame->name,
                                             .parent_prefix = frame->namespace_prefix},
                &name);
            element_map_answer spreads = {.is_true = frame->layout == SXML_ELEMENT_MAP_LAYOUT_LIST_PLUS};
            if (status == SXML_STATUS_OK && !spreads.is_true) {
                status = element_map_members_match(
                    building, (element_map_match_request){.value = role.entry.value,
                                                          .name = name.name},
                    &spreads);
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (spreads.is_true) {
                frame->is_spreading = true;
                frame->spread_array = role.entry.value.content;
                frame->spread_member = 0;
                frame->spread_name = name;
                return sxml_array_get_size(frame->spread_array, &frame->spread_count);
            }
            const element_map_frame child = element_map_child_frame(name, role.entry.value);
            return element_map_push_frame(building, &child);
        }
        is_finished->is_true = status == SXML_STATUS_OK;
        return status;
    }
    size_t member_count = 0;
    if (value_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_ARRAY) {
        status = sxml_array_get_size(value_class.item, &member_count);
    }
    if (status != SXML_STATUS_OK || frame->position >= member_count) {
        is_finished->is_true = status == SXML_STATUS_OK;
        return status;
    }
    sxml_sequence member = sxml_sequence_empty();
    status = sxml_array_get_member(value_class.item, frame->position, &member);
    frame->position += 1;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (frame->layout == SXML_ELEMENT_MAP_LAYOUT_LIST) {
        const element_map_frame child = element_map_child_frame(
            (element_map_decoded_name){.name = frame->entry->child_name,
                                       .namespace_prefix = {.index = 0}},
            member);
        return element_map_push_frame(building, &child);
    }
    const element_map_value_class member_class = element_map_classify(context, member);
    if (member_class.value_kind == SXML_ELEMENT_MAP_VALUE_KIND_STRING) {
        if (frame->has_previous_string) {
            /* Two adjacent strings (F&O 4.0, section 14.6.12.2). */
            return element_map_report(context, &element_map_form_error);
        }
        frame->has_previous_string = true;
        return element_map_send_text(building, member);
    }
    frame->has_previous_string = false;
    element_map_roles roles;
    element_map_entry_role role;
    status = element_map_describe_roles(
            context, (element_map_roles_request){.map = member_class.item, .is_array_member = true},
            &roles);
    if (status == SXML_STATUS_OK) {
        status = element_map_role_of(&roles, 0, &role);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!role.has_text_key || role.role == SXML_ELEMENT_MAP_ROLE_ATTRIBUTE ||
        role.role == SXML_ELEMENT_MAP_ROLE_CONTENT) {
        /* An attribute or the nilled marker after other content. */
        return element_map_report(context, &element_map_form_error);
    }
    if (role.role != SXML_ELEMENT_MAP_ROLE_ELEMENT) {
        return element_map_send_node(building, &role);
    }
    element_map_decoded_name name;
    status = element_map_decode_name(
        building,
        (element_map_decode_request){.text = role.name_text,
                                     .parent_name = frame->name,
                                     .parent_prefix = frame->namespace_prefix},
        &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const element_map_frame child = element_map_child_frame(name, role.entry.value);
    return element_map_push_frame(building, &child);
}

/* The statically known namespaces of the code running: those of the
   unit of the innermost task that runs IR (as the static base URI, D91). */
static sxml_namespace_binding_view element_map_static_namespaces(const sxml_function_call *call) {
    if (call->dynamic_context == NULL) {
        return (sxml_namespace_binding_view){NULL, 0};
    }
    const sxml_evaluator_task_stack *stack = &call->dynamic_context->task_stack;
    for (size_t task_count = stack->task_count; task_count > 0; task_count -= 1) {
        const sxml_ir_function *function = stack->tasks[task_count - 1].function;
        if (function != NULL && function->unit != NULL) {
            return sxml_ir_unit_get_static_namespaces(function->unit);
        }
    }
    return (sxml_namespace_binding_view){NULL, 0};
}

/* Builds the element of the single-entry map item into building's tree. */
static sxml_status element_map_build_element(element_map_building *building, sxml_item map) {
    const element_map_context *context = building->context;
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    sxml_map_entry entry;
    sxml_byte_view key_text;
    if (status == SXML_STATUS_OK && entry_count != 1) {
        return element_map_report(context, &element_map_form_error);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_map_get_entry(map, 0, &entry);
    }
    if (status == SXML_STATUS_OK && !element_map_item_text(entry.key, &key_text)) {
        return element_map_report(context, &element_map_form_error);
    }
    element_map_decoded_name name;
    if (status == SXML_STATUS_OK) {
        status = element_map_decode_name(
            building, (element_map_decode_request){.text = key_text, .is_top = true}, &name);
    }
    if (status == SXML_STATUS_OK) {
        element_map_frame top = element_map_child_frame(name, entry.value);
        top.is_top = true;
        status = element_map_push_frame(building, &top);
    }
    while (status == SXML_STATUS_OK && building->frames.element_count > 0) {
        element_map_frame *frame =
            &((element_map_frame *)building->frames.elements)[building->frames.element_count - 1];
        element_map_answer is_finished = {.is_true = false};
        if (!frame->is_started) {
            status = element_map_start_frame(building, &is_finished);
        } else {
            status = element_map_continue_frame(building, &is_finished);
            if (status == SXML_STATUS_OK && is_finished.is_true) {
                status = building->builder.operations->end_element(building->builder.state);
            }
        }
        if (status == SXML_STATUS_OK && is_finished.is_true) {
            building->frames.element_count -= 1;
        }
    }
    return status;
}

/* fn:map-to-element($map as map(*)?, $options as map(*)? := {}) as
   element()? (F&O 4.0, section 14.6.15): a new parentless element in a
   tree of its own, made in heap memory, that destroys itself when
   unreferenced. */
static sxml_status call_map_to_element(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    *result = sxml_sequence_empty();
    element_map_context context = element_map_start_context(call);
    sxml_status status = element_map_read_options(&context);
    if (status != SXML_STATUS_OK || element_map_is_empty(input)) {
        element_map_context_release(&context);
        return status;
    }
    const sxml_allocator tree_allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_arena_tree *arena_tree = NULL;
    status = sxml_arena_tree_create(
        tree_allocator,
        &(sxml_arena_tree_configuration){
            .allocator = tree_allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = call->item_heap,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = call->error_record,
            .base_uri = call->dynamic_context == NULL
                            ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                            : sxml_dynamic_context_get_static_base_uri(call->dynamic_context)},
        &arena_tree);
    element_map_building building = {.context = &context,
                                     .static_namespaces = element_map_static_namespaces(call),
                                     .prefixed_namespaces = {NULL, 0, 0},
                                     .attribute_names = {NULL, 0, 0},
                                     .frames = {NULL, 0, 0}};
    if (status == SXML_STATUS_OK) {
        building.builder = sxml_arena_tree_as_tree_builder(arena_tree);
        status = element_map_build_element(&building, input.content);
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    element_map_buffer_release(&context, &building.prefixed_namespaces);
    element_map_buffer_release(&context, &building.attribute_names);
    element_map_buffer_release(&context, &building.frames);
    element_map_context_release(&context);
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------------- Rows */

static const sxml_function_parameter element_to_map_plan_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "node()*"),
};

/* The choice types of the first parameters are checked by the bodies
   (D78: the evaluator does not coerce into choice types). */
static const sxml_function_parameter element_to_map_parameters[] = {
    SXML_REQUIRED_PARAMETER("element", "node()?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_function_parameter map_to_element_parameters[] = {
    SXML_REQUIRED_PARAMETER("map", "map(*)?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_native_function element_map_functions[] = {
    SXML_FUNCTION_ROW("element-to-map-plan", element_to_map_plan_parameters,
                      "map(xs:string, map(*))", 0, call_element_to_map_plan),
    SXML_FUNCTION_ROW("element-to-map", element_to_map_parameters, "map(xs:string, item()?)?", 0,
                      call_element_to_map),
    SXML_FUNCTION_ROW("map-to-element", map_to_element_parameters, "element()?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                          SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
                      call_map_to_element),
};

const sxml_native_function_view sxml_function_library_element_map =
    SXML_LIBRARY_PART(element_map_functions);
