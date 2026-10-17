/*
 * What the HTML output method adds to the node writer of
 * src/serialize_xml.c (Serialization 4.0, section 7 "HTML Output Method";
 * docs/DECISIONS.md D120, D128): the elements with an empty content model
 * (also used by the XHTML method, section 6), raw text elements, boolean
 * attributes in minimized form, and HTML processing instructions.
 *
 * Golden file: src/serialize_xml.c (tables of names, the pipeline).
 */
#include "serialize_internal.h"

#include "sxml/view.h"

/* ------------------------------------------------------------- Tables */

/* Section 7.1: the void elements of HTML5. */
static const sxml_byte_view sxml_html5_void_element_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("area"),   SXML_BYTE_VIEW_INITIALIZER("base"),
    SXML_BYTE_VIEW_INITIALIZER("br"),     SXML_BYTE_VIEW_INITIALIZER("col"),
    SXML_BYTE_VIEW_INITIALIZER("embed"),  SXML_BYTE_VIEW_INITIALIZER("hr"),
    SXML_BYTE_VIEW_INITIALIZER("img"),    SXML_BYTE_VIEW_INITIALIZER("input"),
    SXML_BYTE_VIEW_INITIALIZER("keygen"), SXML_BYTE_VIEW_INITIALIZER("link"),
    SXML_BYTE_VIEW_INITIALIZER("meta"),   SXML_BYTE_VIEW_INITIALIZER("param"),
    SXML_BYTE_VIEW_INITIALIZER("source"), SXML_BYTE_VIEW_INITIALIZER("track"),
    SXML_BYTE_VIEW_INITIALIZER("wbr"),
};

/* Section 7.1: the elements of HTML 4.0 with an empty content model (the
   XHTML method's EMPTY elements, section 6, are the same). */
static const sxml_byte_view sxml_html4_empty_element_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("area"),     SXML_BYTE_VIEW_INITIALIZER("base"),
    SXML_BYTE_VIEW_INITIALIZER("basefont"), SXML_BYTE_VIEW_INITIALIZER("br"),
    SXML_BYTE_VIEW_INITIALIZER("col"),      SXML_BYTE_VIEW_INITIALIZER("embed"),
    SXML_BYTE_VIEW_INITIALIZER("frame"),    SXML_BYTE_VIEW_INITIALIZER("hr"),
    SXML_BYTE_VIEW_INITIALIZER("img"),      SXML_BYTE_VIEW_INITIALIZER("input"),
    SXML_BYTE_VIEW_INITIALIZER("isindex"),  SXML_BYTE_VIEW_INITIALIZER("link"),
    SXML_BYTE_VIEW_INITIALIZER("meta"),     SXML_BYTE_VIEW_INITIALIZER("param"),
};

/* Section 7.2: the boolean attributes of the HTML 4.01 DTDs and of HTML5
   (HTML Living Standard, "Boolean attributes"). */
static const sxml_byte_view sxml_html_boolean_attribute_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("allowfullscreen"), SXML_BYTE_VIEW_INITIALIZER("async"),
    SXML_BYTE_VIEW_INITIALIZER("autofocus"),       SXML_BYTE_VIEW_INITIALIZER("autoplay"),
    SXML_BYTE_VIEW_INITIALIZER("checked"),         SXML_BYTE_VIEW_INITIALIZER("compact"),
    SXML_BYTE_VIEW_INITIALIZER("controls"),        SXML_BYTE_VIEW_INITIALIZER("declare"),
    SXML_BYTE_VIEW_INITIALIZER("default"),         SXML_BYTE_VIEW_INITIALIZER("defer"),
    SXML_BYTE_VIEW_INITIALIZER("disabled"),        SXML_BYTE_VIEW_INITIALIZER("formnovalidate"),
    SXML_BYTE_VIEW_INITIALIZER("hidden"),          SXML_BYTE_VIEW_INITIALIZER("inert"),
    SXML_BYTE_VIEW_INITIALIZER("ismap"),           SXML_BYTE_VIEW_INITIALIZER("itemscope"),
    SXML_BYTE_VIEW_INITIALIZER("loop"),            SXML_BYTE_VIEW_INITIALIZER("multiple"),
    SXML_BYTE_VIEW_INITIALIZER("muted"),           SXML_BYTE_VIEW_INITIALIZER("nohref"),
    SXML_BYTE_VIEW_INITIALIZER("noresize"),        SXML_BYTE_VIEW_INITIALIZER("noshade"),
    SXML_BYTE_VIEW_INITIALIZER("novalidate"),      SXML_BYTE_VIEW_INITIALIZER("nowrap"),
    SXML_BYTE_VIEW_INITIALIZER("open"),            SXML_BYTE_VIEW_INITIALIZER("playsinline"),
    SXML_BYTE_VIEW_INITIALIZER("readonly"),        SXML_BYTE_VIEW_INITIALIZER("required"),
    SXML_BYTE_VIEW_INITIALIZER("reversed"),        SXML_BYTE_VIEW_INITIALIZER("selected"),
};

/* Whether text equals name (lower case) without regard to ASCII case. */
static bool sxml_html_name_equals(sxml_byte_view text, sxml_byte_view name) {
    if (text.byte_count != name.byte_count) {
        return false;
    }
    for (size_t offset = 0; offset < name.byte_count; offset += 1) {
        uint8_t byte = text.bytes[offset];
        if (byte >= 'A' && byte <= 'Z') {
            byte = (uint8_t)(byte - 'A' + 'a');
        }
        if (byte != name.bytes[offset]) {
            return false;
        }
    }
    return true;
}

typedef struct sxml_html_name_table {
    const sxml_byte_view *names;
    size_t name_count;
} sxml_html_name_table;

static bool sxml_html_name_table_contains(sxml_html_name_table table, sxml_byte_view name) {
    for (size_t name_index = 0; name_index < table.name_count; name_index += 1) {
        if (sxml_html_name_equals(name, table.names[name_index])) {
            return true;
        }
    }
    return false;
}

#define SXML_HTML_NAME_TABLE(array)                                            \
    ((sxml_html_name_table){.names = (array),                                  \
                            .name_count = sizeof(array) / sizeof((array)[0])})

/* ------------------------------------------------------------ Elements */

bool sxml_html_element_is_void(sxml_html_element_query query) {
    return query.is_html5
               ? sxml_html_name_table_contains(SXML_HTML_NAME_TABLE(sxml_html5_void_element_names),
                                               query.local_name)
               : sxml_html_name_table_contains(SXML_HTML_NAME_TABLE(sxml_html4_empty_element_names),
                                               query.local_name);
}

bool sxml_html_element_has_raw_text(sxml_byte_view local_name) {
    return sxml_html_name_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("script")) ||
           sxml_html_name_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("style"));
}

/* ---------------------------------------------------------- Attributes */

bool sxml_html_attribute_is_minimized(sxml_html_attribute_query query) {
    if (!sxml_html_name_table_contains(SXML_HTML_NAME_TABLE(sxml_html_boolean_attribute_names),
                                       query.attribute_name)) {
        return false;
    }
    /* The value equals the name without regard to case. */
    if (query.value.byte_count != query.attribute_name.byte_count) {
        return false;
    }
    for (size_t offset = 0; offset < query.value.byte_count; offset += 1) {
        uint8_t first = query.value.bytes[offset];
        uint8_t second = query.attribute_name.bytes[offset];
        first = first >= 'A' && first <= 'Z' ? (uint8_t)(first - 'A' + 'a') : first;
        second = second >= 'A' && second <= 'Z' ? (uint8_t)(second - 'A' + 'a') : second;
        if (first != second) {
            return false;
        }
    }
    return true;
}

/* ----------------------------------------------- Processing instructions */

/* Writes text with a space between two adjacent hyphens (section 7.4). */
static sxml_status sxml_html_write_separated(sxml_serializer *serializer, sxml_byte_view text) {
    size_t run_start = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t offset = 1; status == SXML_STATUS_OK && offset < text.byte_count; offset += 1) {
        if (text.bytes[offset] == '-' && text.bytes[offset - 1] == '-') {
            status = sxml_serializer_write_markup(
                serializer,
                (sxml_byte_view){.bytes = text.bytes + run_start, .byte_count = offset - run_start});
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
            }
            run_start = offset;
        }
    }
    if (status == SXML_STATUS_OK && run_start < text.byte_count) {
        status = sxml_serializer_write_markup(
            serializer, (sxml_byte_view){.bytes = text.bytes + run_start,
                                         .byte_count = text.byte_count - run_start});
    }
    return status;
}

sxml_status sxml_serializer_write_html_processing_instruction(
    sxml_serializer *serializer, const sxml_html_processing_instruction *instruction) {
    const bool has_data = instruction->data.byte_count > 0;
    if (!instruction->is_html5) {
        for (size_t offset = 0; offset < instruction->data.byte_count; offset += 1) {
            if (instruction->data.bytes[offset] == '>') {
                return sxml_serializer_raise_error(
                    serializer,
                    (sxml_serialization_error){
                        .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SERE0015"),
                        .description = SXML_BYTE_VIEW_FROM_LITERAL(
                            "a processing instruction written by the HTML method contains \">\"")});
            }
        }
        sxml_status status = sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL("<?"));
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write_markup(serializer, instruction->target);
        }
        if (status == SXML_STATUS_OK && has_data) {
            status = sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_write_markup(serializer, instruction->data);
            }
        }
        return status == SXML_STATUS_OK
                   ? sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL(">"))
                   : status;
    }
    /* HTML5 has no processing instructions: a comment (section 7.4). */
    sxml_status status = sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL("<!--?"));
    if (status == SXML_STATUS_OK) {
        status = sxml_html_write_separated(serializer, instruction->target);
    }
    if (status == SXML_STATUS_OK && has_data) {
        status = sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
        if (status == SXML_STATUS_OK) {
            status = sxml_html_write_separated(serializer, instruction->data);
        }
    }
    return status == SXML_STATUS_OK
               ? sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL("?-->"))
               : status;
}
