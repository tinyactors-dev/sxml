/*
 * The serialization parameter reader (include/sxml/serialize.h;
 * docs/DECISIONS.md D106, D128): fn:serialize's $options, either a map
 * with the option parameter conventions (F&O 4.0, 17.2.3 "fn:serialize")
 * or a parameter document, an output:serialization-parameters element
 * (Serialization 4.0, 3.1 "Serialization Parameter Documents"), into an
 * sxml_serialization_parameters.
 *
 * Both forms decode each parameter into one sxml_parameter_value (a flag,
 * a text, a name list or a character map) and assign it the same way; only
 * the error codes differ (XPTY0004 and SEPM0016 for a map, SEPM0017 to
 * SEPM0019 for an element). What the read parameters refer to lives in
 * one of two arenas: a read fills the spare one and, when it succeeds,
 * makes it current, so a failed read leaves the last parameters valid.
 *
 * Golden file: src/memory.c (lifecycle); src/function_library_json.c (the
 * option parameter conventions).
 */
#include "sxml/serialize.h"

#include "serialize_internal.h"
#include "sxml/name.h"
#include "sxml/string.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

struct sxml_serialization_parameter_reader {
    sxml_allocator allocator;
    sxml_allocator creating_allocator;
    sxml_item_heap *item_heap;
    /* What the last successful read refers to, and the arena the next
       read fills; both made at the first read. */
    sxml_arena *current_arena;
    sxml_arena *spare_arena;
};

/* ------------------------------------------------------------ Lifecycle */

sxml_status sxml_serialization_parameter_reader_memory_requirements(
    const sxml_serialization_parameter_reader_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || requirements == NULL ||
        sxml_allocator_is_null(configuration->allocator) || configuration->item_heap == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements =
        (sxml_memory_requirements){.byte_count = sizeof(sxml_serialization_parameter_reader),
                                   .alignment = alignof(sxml_serialization_parameter_reader)};
    return SXML_STATUS_OK;
}

sxml_status sxml_serialization_parameter_reader_initialize(
    sxml_memory_view memory,
    const sxml_serialization_parameter_reader_configuration *configuration,
    sxml_serialization_parameter_reader **parameter_reader) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_serialization_parameter_reader_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && parameter_reader == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_serialization_parameter_reader *reader = placement;
    *reader = (sxml_serialization_parameter_reader){
        .allocator = configuration->allocator,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .item_heap = configuration->item_heap,
        .current_arena = NULL,
        .spare_arena = NULL,
    };
    *parameter_reader = reader;
    return SXML_STATUS_OK;
}

void sxml_serialization_parameter_reader_finalize(
    sxml_serialization_parameter_reader *parameter_reader) {
    if (parameter_reader == NULL) {
        return;
    }
    sxml_arena_destroy(parameter_reader->current_arena);
    sxml_arena_destroy(parameter_reader->spare_arena);
    parameter_reader->current_arena = NULL;
    parameter_reader->spare_arena = NULL;
}

sxml_status sxml_serialization_parameter_reader_create(
    sxml_allocator allocator,
    const sxml_serialization_parameter_reader_configuration *configuration,
    sxml_serialization_parameter_reader **parameter_reader) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_serialization_parameter_reader_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && parameter_reader == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_serialization_parameter_reader *reader = NULL;
    status = sxml_serialization_parameter_reader_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &reader);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    reader->creating_allocator = allocator;
    *parameter_reader = reader;
    return SXML_STATUS_OK;
}

void sxml_serialization_parameter_reader_destroy(
    sxml_serialization_parameter_reader *parameter_reader) {
    if (parameter_reader == NULL) {
        return;
    }
    const sxml_allocator creating_allocator = parameter_reader->creating_allocator;
    sxml_serialization_parameter_reader_finalize(parameter_reader);
    sxml_allocator_release(creating_allocator, parameter_reader);
}

/* ------------------------------------------------------------ Parameters */

/* The parameters of Serialization 4.0, section 3, in the order of its
   table. */
typedef enum sxml_parameter_name {
    SXML_PARAMETER_ALLOW_DUPLICATE_NAMES,
    SXML_PARAMETER_BYTE_ORDER_MARK,
    SXML_PARAMETER_CANONICAL,
    SXML_PARAMETER_CDATA_SECTION_ELEMENTS,
    SXML_PARAMETER_CSV_HEADER,
    SXML_PARAMETER_CSV_QUOTE_CHARACTER,
    SXML_PARAMETER_CSV_SEPARATOR,
    SXML_PARAMETER_DOCTYPE_PUBLIC,
    SXML_PARAMETER_DOCTYPE_SYSTEM,
    SXML_PARAMETER_ENCODING,
    SXML_PARAMETER_ESCAPE_SOLIDUS,
    SXML_PARAMETER_ESCAPE_URI_ATTRIBUTES,
    SXML_PARAMETER_HTML_VERSION,
    SXML_PARAMETER_INCLUDE_CONTENT_TYPE,
    SXML_PARAMETER_INDENT,
    SXML_PARAMETER_INDENT_ATTRIBUTES,
    SXML_PARAMETER_INDENT_UNIT,
    SXML_PARAMETER_ITEM_SEPARATOR,
    SXML_PARAMETER_JSON_LINES,
    SXML_PARAMETER_JSON_NODE_OUTPUT_METHOD,
    SXML_PARAMETER_LINE_ENDING,
    SXML_PARAMETER_MEDIA_TYPE,
    SXML_PARAMETER_METHOD,
    SXML_PARAMETER_NORMALIZATION_FORM,
    SXML_PARAMETER_OMIT_XML_DECLARATION,
    SXML_PARAMETER_STANDALONE,
    SXML_PARAMETER_SUPPRESS_INDENTATION,
    SXML_PARAMETER_UNDECLARE_PREFIXES,
    SXML_PARAMETER_USE_CHARACTER_MAPS,
    SXML_PARAMETER_VERSION,
    SXML_PARAMETER_COUNT,
} sxml_parameter_name;

/* How a parameter's value is given (the F&O 4.0 table's required types). */
typedef enum sxml_parameter_kind {
    /* xs:boolean?; yes, no, true, false, 1, 0 in a parameter document. */
    SXML_PARAMETER_KIND_FLAG,
    /* standalone: xs:boolean?, () meaning omit; also omit in a document. */
    SXML_PARAMETER_KIND_STANDALONE,
    /* xs:string?. */
    SXML_PARAMETER_KIND_TEXT,
    /* (xs:string | xs:QName)?: method and json-node-output-method. */
    SXML_PARAMETER_KIND_METHOD,
    /* xs:decimal?: html-version. */
    SXML_PARAMETER_KIND_DECIMAL,
    /* xs:QName*: cdata-section-elements, suppress-indentation. */
    SXML_PARAMETER_KIND_NAMES,
    /* map(xs:string, xs:string)?: use-character-maps. */
    SXML_PARAMETER_KIND_CHARACTER_MAP,
} sxml_parameter_kind;

typedef struct sxml_parameter_definition {
    sxml_byte_view name;
    sxml_parameter_kind kind;
} sxml_parameter_definition;

#define SXML_PARAMETER(name_literal, kind_constant)                            \
    {.name = SXML_BYTE_VIEW_INITIALIZER(name_literal), .kind = (kind_constant)}

static const sxml_parameter_definition sxml_parameter_definitions[SXML_PARAMETER_COUNT] = {
    SXML_PARAMETER("allow-duplicate-names", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("byte-order-mark", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("canonical", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("cdata-section-elements", SXML_PARAMETER_KIND_NAMES),
    SXML_PARAMETER("csv-header", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("csv-quote-character", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("csv-separator", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("doctype-public", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("doctype-system", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("encoding", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("escape-solidus", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("escape-uri-attributes", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("html-version", SXML_PARAMETER_KIND_DECIMAL),
    SXML_PARAMETER("include-content-type", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("indent", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("indent-attributes", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("indent-unit", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("item-separator", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("json-lines", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("json-node-output-method", SXML_PARAMETER_KIND_METHOD),
    SXML_PARAMETER("line-ending", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("media-type", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("method", SXML_PARAMETER_KIND_METHOD),
    SXML_PARAMETER("normalization-form", SXML_PARAMETER_KIND_TEXT),
    SXML_PARAMETER("omit-xml-declaration", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("standalone", SXML_PARAMETER_KIND_STANDALONE),
    SXML_PARAMETER("suppress-indentation", SXML_PARAMETER_KIND_NAMES),
    SXML_PARAMETER("undeclare-prefixes", SXML_PARAMETER_KIND_FLAG),
    SXML_PARAMETER("use-character-maps", SXML_PARAMETER_KIND_CHARACTER_MAP),
    SXML_PARAMETER("version", SXML_PARAMETER_KIND_TEXT),
};

/* Finds the parameter named name; SXML_PARAMETER_COUNT for none. */
static sxml_parameter_name sxml_parameter_find(sxml_byte_view name) {
    for (size_t parameter_index = 0; parameter_index < SXML_PARAMETER_COUNT;
         parameter_index += 1) {
        if (sxml_byte_view_equals(sxml_parameter_definitions[parameter_index].name, name)) {
            return (sxml_parameter_name)parameter_index;
        }
    }
    return SXML_PARAMETER_COUNT;
}

/* One decoded parameter value. */
typedef struct sxml_parameter_value {
    /* The value is () (the default; for standalone, omit). */
    bool is_absent;
    /* FLAG and STANDALONE. */
    bool flag;
    /* TEXT, DECIMAL and METHOD (for a method name given as a string). */
    sxml_byte_view text;
    /* METHOD given as a QName in a namespace: an implementation-defined
       method, which sxml has none of. */
    bool is_qualified_method;
    /* NAMES, in the reading arena. */
    sxml_element_name_view names;
    /* CHARACTER_MAP, in the reading arena. */
    sxml_character_map_view character_map;
} sxml_parameter_value;

/* What one read works with. */
typedef struct sxml_parameter_reading {
    sxml_serialization_parameter_reader *reader;
    sxml_arena *arena;
    sxml_error_record *error_record;
    sxml_serialization_parameters parameters;
    /* Element form: the codes of 3.1 instead of XPTY0004 and SEPM0016. */
    bool is_document;
} sxml_parameter_reading;

/* An error to raise: a code of the err namespace and a description, both
   string literals. */
typedef struct sxml_parameter_error {
    const char *code;
    const char *description;
    sxml_status status;
} sxml_parameter_error;

static sxml_status sxml_parameter_raise(sxml_parameter_reading *reading,
                                        sxml_parameter_error error) {
    if (reading->error_record != NULL) {
        *reading->error_record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = sxml_byte_view_from_c_string(error.code),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .value = NULL,
            .diagnostic = {.description = sxml_byte_view_from_c_string(error.description),
                           .location = {.line_number = 0}},
        };
    }
    return error.status;
}

/* A value of the wrong type: XPTY0004 in a map, SEPM0017 in a document. */
static sxml_status sxml_parameter_raise_type(sxml_parameter_reading *reading) {
    return reading->is_document
               ? sxml_parameter_raise(reading,
                                      (sxml_parameter_error){
                                          .code = "SEPM0017",
                                          .description = "a parameter document holds an invalid "
                                                         "serialization parameter",
                                          .status = SXML_STATUS_DYNAMIC_ERROR})
               : sxml_parameter_raise(reading, (sxml_parameter_error){
                                                   .code = "XPTY0004",
                                                   .description = "a serialization option has a "
                                                                  "value of the wrong type",
                                                   .status = SXML_STATUS_TYPE_ERROR});
}

/* A value the parameter does not permit: SEPM0016 (Serialization 4.0,
   section 3), SEPM0017 when a document's schema already forbids it. */
typedef struct sxml_invalid_value {
    const char *description;
    bool is_schema_invalid;
} sxml_invalid_value;

static sxml_status sxml_parameter_raise_invalid(sxml_parameter_reading *reading,
                                                sxml_invalid_value invalid) {
    return sxml_parameter_raise(
        reading,
        (sxml_parameter_error){.code = reading->is_document && invalid.is_schema_invalid
                                           ? "SEPM0017"
                                           : "SEPM0016",
                               .description = invalid.description,
                               .status = SXML_STATUS_DYNAMIC_ERROR});
}

/* Copies text into the reading arena. */
static sxml_status sxml_parameter_copy_text(sxml_parameter_reading *reading, sxml_byte_view text,
                                            sxml_byte_view *copy) {
    if (text.byte_count == 0) {
        *copy = (sxml_byte_view){.bytes = (const uint8_t *)"", .byte_count = 0};
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    const sxml_status status = sxml_arena_allocate(
        reading->arena, (sxml_allocation_request){.byte_count = text.byte_count, .alignment = 1},
        &memory);
    if (status == SXML_STATUS_OK) {
        memcpy(memory, text.bytes, text.byte_count);
        *copy = (sxml_byte_view){.bytes = memory, .byte_count = text.byte_count};
    }
    return status;
}

/* text without leading and trailing XML whitespace (the whitespace
   collapse of xs:token, Serialization 4.0, appendix B). */
static sxml_byte_view sxml_parameter_trim(sxml_byte_view text) {
    while (text.byte_count > 0 && (text.bytes[0] == ' ' || text.bytes[0] == '\t' ||
                                   text.bytes[0] == '\n' || text.bytes[0] == '\r')) {
        text.bytes += 1;
        text.byte_count -= 1;
    }
    while (text.byte_count > 0 &&
           (text.bytes[text.byte_count - 1] == ' ' || text.bytes[text.byte_count - 1] == '\t' ||
            text.bytes[text.byte_count - 1] == '\n' || text.bytes[text.byte_count - 1] == '\r')) {
        text.byte_count -= 1;
    }
    return text;
}

/* The number of characters of well-formed UTF-8 text, and its first. */
typedef struct sxml_character_count {
    size_t count;
    uint32_t first_codepoint;
} sxml_character_count;

static sxml_character_count sxml_parameter_count_characters(sxml_byte_view text) {
    sxml_character_count result = {.count = 0, .first_codepoint = 0};
    for (size_t offset = 0; offset < text.byte_count; offset += 1) {
        if ((text.bytes[offset] & 0xC0U) != 0x80U) {
            result.count += 1;
        }
    }
    if (text.byte_count > 0) {
        const uint8_t lead = text.bytes[0];
        size_t length = 1;
        uint32_t codepoint = lead;
        if (lead >= 0xF0U) {
            length = 4;
            codepoint = lead & 0x07U;
        } else if (lead >= 0xE0U) {
            length = 3;
            codepoint = lead & 0x0FU;
        } else if (lead >= 0xC0U) {
            length = 2;
            codepoint = lead & 0x1FU;
        }
        for (size_t continuation = 1; continuation < length && continuation < text.byte_count;
             continuation += 1) {
            codepoint = (codepoint << 6) | (text.bytes[continuation] & 0x3FU);
        }
        result.first_codepoint = codepoint;
    }
    return result;
}

/* Whether text equals name without regard to ASCII case. */
static bool sxml_parameter_equals_ignoring_case(sxml_byte_view text, sxml_byte_view expected) {
    if (text.byte_count != expected.byte_count) {
        return false;
    }
    for (size_t offset = 0; offset < text.byte_count; offset += 1) {
        uint8_t first = text.bytes[offset];
        uint8_t second = expected.bytes[offset];
        first = first >= 'A' && first <= 'Z' ? (uint8_t)(first - 'A' + 'a') : first;
        second = second >= 'A' && second <= 'Z' ? (uint8_t)(second - 'A' + 'a') : second;
        if (first != second) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ Assigning */

static sxml_serialization_flag sxml_parameter_flag_of(const sxml_parameter_value *value) {
    return value->flag ? SXML_SERIALIZATION_FLAG_YES : SXML_SERIALIZATION_FLAG_NO;
}

/* A method name: xml, xhtml, html, text, json, csv, adaptive (section 3,
   parameter method). */
static bool sxml_parameter_find_method(sxml_byte_view name, sxml_serialization_method *method) {
    static const struct {
        const char *name;
        sxml_serialization_method method;
    } methods[] = {
        {"xml", SXML_SERIALIZATION_METHOD_XML},   {"xhtml", SXML_SERIALIZATION_METHOD_XHTML},
        {"html", SXML_SERIALIZATION_METHOD_HTML}, {"text", SXML_SERIALIZATION_METHOD_TEXT},
        {"json", SXML_SERIALIZATION_METHOD_JSON}, {"csv", SXML_SERIALIZATION_METHOD_CSV},
        {"adaptive", SXML_SERIALIZATION_METHOD_ADAPTIVE},
    };
    for (size_t method_index = 0; method_index < sizeof(methods) / sizeof(methods[0]);
         method_index += 1) {
        if (sxml_byte_view_equals(name, sxml_byte_view_from_c_string(methods[method_index].name))) {
            *method = methods[method_index].method;
            return true;
        }
    }
    return false;
}

static sxml_status sxml_parameter_assign_method(sxml_parameter_reading *reading,
                                                sxml_parameter_name name,
                                                const sxml_parameter_value *value) {
    sxml_serialization_method method = SXML_SERIALIZATION_METHOD_XML;
    if (value->is_qualified_method) {
        return sxml_parameter_raise_invalid(
            reading, (sxml_invalid_value){.description = "sxml defines no implementation-defined "
                                                         "output method",
                                          .is_schema_invalid = false});
    }
    if (!sxml_parameter_find_method(value->text, &method) ||
        (name == SXML_PARAMETER_JSON_NODE_OUTPUT_METHOD &&
         method != SXML_SERIALIZATION_METHOD_XML && method != SXML_SERIALIZATION_METHOD_XHTML &&
         method != SXML_SERIALIZATION_METHOD_HTML && method != SXML_SERIALIZATION_METHOD_TEXT)) {
        return sxml_parameter_raise_invalid(
            reading, (sxml_invalid_value){.description = "an output method is not one of xml, "
                                                         "xhtml, html, text, json, csv, adaptive",
                                          .is_schema_invalid = true});
    }
    if (name == SXML_PARAMETER_METHOD) {
        reading->parameters.serialization_method = method;
        return SXML_STATUS_OK;
    }
    reading->parameters.json_node_output_method =
        method == SXML_SERIALIZATION_METHOD_XHTML  ? SXML_JSON_NODE_OUTPUT_METHOD_XHTML
        : method == SXML_SERIALIZATION_METHOD_HTML ? SXML_JSON_NODE_OUTPUT_METHOD_HTML
        : method == SXML_SERIALIZATION_METHOD_TEXT ? SXML_JSON_NODE_OUTPUT_METHOD_TEXT
                                                   : SXML_JSON_NODE_OUTPUT_METHOD_XML;
    return SXML_STATUS_OK;
}

/* encoding (Serialization 4.0, 5.1.3, and serialize.h's aliases):
   SESU0007 for an encoding sxml does not write. */
static sxml_status sxml_parameter_assign_encoding(sxml_parameter_reading *reading,
                                                  sxml_byte_view text) {
    static const struct {
        const char *name;
        sxml_character_encoding encoding;
    } encodings[] = {
        {"UTF-8", SXML_CHARACTER_ENCODING_UTF_8},
        {"UTF8", SXML_CHARACTER_ENCODING_UTF_8},
        {"UTF-16", SXML_CHARACTER_ENCODING_UTF_16},
        {"UTF16", SXML_CHARACTER_ENCODING_UTF_16},
        {"US-ASCII", SXML_CHARACTER_ENCODING_US_ASCII},
        {"ASCII", SXML_CHARACTER_ENCODING_US_ASCII},
        {"ISO-8859-1", SXML_CHARACTER_ENCODING_ISO_8859_1},
        {"ISO_8859-1", SXML_CHARACTER_ENCODING_ISO_8859_1},
        {"latin1", SXML_CHARACTER_ENCODING_ISO_8859_1},
    };
    for (size_t encoding_index = 0; encoding_index < sizeof(encodings) / sizeof(encodings[0]);
         encoding_index += 1) {
        if (sxml_parameter_equals_ignoring_case(
                text, sxml_byte_view_from_c_string(encodings[encoding_index].name))) {
            reading->parameters.character_encoding = encodings[encoding_index].encoding;
            return SXML_STATUS_OK;
        }
    }
    return sxml_parameter_raise(
        reading, (sxml_parameter_error){.code = "SESU0007",
                                        .description = "the serializer does not support this "
                                                       "output encoding",
                                        .status = SXML_STATUS_DYNAMIC_ERROR});
}

/* normalization-form (5.1.12): SESU0011 for a form sxml does not know. */
static sxml_status sxml_parameter_assign_normalization(sxml_parameter_reading *reading,
                                                       sxml_byte_view text) {
    static const struct {
        const char *name;
        sxml_normalization_form form;
    } forms[] = {
        {"none", SXML_NORMALIZATION_FORM_NONE},
        {"NFC", SXML_NORMALIZATION_FORM_NFC},
        {"NFD", SXML_NORMALIZATION_FORM_NFD},
        {"NFKC", SXML_NORMALIZATION_FORM_NFKC},
        {"NFKD", SXML_NORMALIZATION_FORM_NFKD},
        {"fully-normalized", SXML_NORMALIZATION_FORM_FULLY_NORMALIZED},
    };
    const sxml_byte_view trimmed = sxml_parameter_trim(text);
    for (size_t form_index = 0; form_index < sizeof(forms) / sizeof(forms[0]); form_index += 1) {
        if (sxml_byte_view_equals(trimmed, sxml_byte_view_from_c_string(forms[form_index].name))) {
            reading->parameters.normalization_form = forms[form_index].form;
            return SXML_STATUS_OK;
        }
    }
    return sxml_parameter_raise(
        reading, (sxml_parameter_error){.code = "SESU0011",
                                        .description = "the serializer does not support this "
                                                       "normalization form",
                                        .status = SXML_STATUS_DYNAMIC_ERROR});
}

/* line-ending (5.1.7): \r\n, \r or \n, literally or as escapes. */
static sxml_status sxml_parameter_assign_line_ending(sxml_parameter_reading *reading,
                                                     sxml_byte_view text) {
    static const struct {
        const char *text;
        sxml_line_ending line_ending;
    } endings[] = {
        {"\n", SXML_LINE_ENDING_LINE_FEED},
        {"\\n", SXML_LINE_ENDING_LINE_FEED},
        {"\r\n", SXML_LINE_ENDING_CARRIAGE_RETURN_LINE_FEED},
        {"\\r\\n", SXML_LINE_ENDING_CARRIAGE_RETURN_LINE_FEED},
        {"\r", SXML_LINE_ENDING_CARRIAGE_RETURN},
        {"\\r", SXML_LINE_ENDING_CARRIAGE_RETURN},
    };
    for (size_t ending_index = 0; ending_index < sizeof(endings) / sizeof(endings[0]);
         ending_index += 1) {
        if (sxml_byte_view_equals(text, sxml_byte_view_from_c_string(endings[ending_index].text))) {
            reading->parameters.line_ending = endings[ending_index].line_ending;
            return SXML_STATUS_OK;
        }
    }
    return sxml_parameter_raise_invalid(
        reading, (sxml_invalid_value){.description = "line-ending must be a newline, a carriage "
                                                     "return, or both",
                                      .is_schema_invalid = false});
}

/* indent-unit (5.1.5): the escape \t becomes a tab; the serializer
   checks the result. */
static sxml_status sxml_parameter_assign_indent_unit(sxml_parameter_reading *reading,
                                                     sxml_byte_view text) {
    void *memory = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (text.byte_count > 0) {
        status = sxml_arena_allocate(
            reading->arena,
            (sxml_allocation_request){.byte_count = text.byte_count, .alignment = 1}, &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *unit = memory;
    size_t unit_count = 0;
    for (size_t offset = 0; offset < text.byte_count; offset += 1) {
        if (text.bytes[offset] == '\\' && offset + 1 < text.byte_count &&
            text.bytes[offset + 1] == 't') {
            unit[unit_count++] = '\t';
            offset += 1;
        } else {
            unit[unit_count++] = text.bytes[offset];
        }
    }
    reading->parameters.has_indent_unit = true;
    reading->parameters.indent_unit =
        (sxml_byte_view){.bytes = unit_count > 0 ? unit : (const uint8_t *)"",
                         .byte_count = unit_count};
    return SXML_STATUS_OK;
}

/* csv-separator and csv-quote-character (10.3.1, 10.3.2): exactly one
   character. */
static sxml_status sxml_parameter_assign_codepoint(sxml_parameter_reading *reading,
                                                   sxml_parameter_name name,
                                                   sxml_byte_view text) {
    const sxml_character_count characters = sxml_parameter_count_characters(text);
    if (characters.count != 1) {
        return sxml_parameter_raise_invalid(
            reading,
            (sxml_invalid_value){.description = "csv-separator and csv-quote-character must be "
                                                "one character",
                                 .is_schema_invalid = false});
    }
    if (name == SXML_PARAMETER_CSV_SEPARATOR) {
        reading->parameters.csv_separator_codepoint = characters.first_codepoint;
    } else {
        reading->parameters.csv_quote_character_codepoint = characters.first_codepoint;
    }
    return SXML_STATUS_OK;
}

/* Where a flag parameter, or a parameter that may be absent, is stored
   in sxml_serialization_parameters. */
typedef struct sxml_parameter_field {
    sxml_parameter_name name;
    size_t field_offset;
    /* For a parameter that may be absent: its has_ field. */
    size_t presence_offset;
} sxml_parameter_field;

#define SXML_FLAG_FIELD(constant, field)                                       \
    {.name = (constant),                                                       \
     .field_offset = offsetof(sxml_serialization_parameters, field),           \
     .presence_offset = 0}

#define SXML_TEXT_FIELD(constant, field)                                       \
    {.name = (constant),                                                       \
     .field_offset = offsetof(sxml_serialization_parameters, field),           \
     .presence_offset = offsetof(sxml_serialization_parameters, has_##field)}

static const sxml_parameter_field sxml_flag_fields[] = {
    SXML_FLAG_FIELD(SXML_PARAMETER_ALLOW_DUPLICATE_NAMES, allow_duplicate_names),
    SXML_FLAG_FIELD(SXML_PARAMETER_BYTE_ORDER_MARK, byte_order_mark),
    SXML_FLAG_FIELD(SXML_PARAMETER_CANONICAL, canonical),
    SXML_FLAG_FIELD(SXML_PARAMETER_CSV_HEADER, csv_header),
    SXML_FLAG_FIELD(SXML_PARAMETER_ESCAPE_SOLIDUS, escape_solidus),
    SXML_FLAG_FIELD(SXML_PARAMETER_ESCAPE_URI_ATTRIBUTES, escape_uri_attributes),
    SXML_FLAG_FIELD(SXML_PARAMETER_INCLUDE_CONTENT_TYPE, include_content_type),
    SXML_FLAG_FIELD(SXML_PARAMETER_INDENT, indent),
    SXML_FLAG_FIELD(SXML_PARAMETER_INDENT_ATTRIBUTES, indent_attributes),
    SXML_FLAG_FIELD(SXML_PARAMETER_JSON_LINES, json_lines),
    SXML_FLAG_FIELD(SXML_PARAMETER_OMIT_XML_DECLARATION, omit_xml_declaration),
    SXML_FLAG_FIELD(SXML_PARAMETER_UNDECLARE_PREFIXES, undeclare_prefixes),
};

static const sxml_parameter_field sxml_text_fields[] = {
    SXML_TEXT_FIELD(SXML_PARAMETER_DOCTYPE_PUBLIC, doctype_public),
    SXML_TEXT_FIELD(SXML_PARAMETER_DOCTYPE_SYSTEM, doctype_system),
    SXML_TEXT_FIELD(SXML_PARAMETER_HTML_VERSION, html_version),
    SXML_TEXT_FIELD(SXML_PARAMETER_ITEM_SEPARATOR, item_separator),
    SXML_TEXT_FIELD(SXML_PARAMETER_MEDIA_TYPE, media_type),
    SXML_TEXT_FIELD(SXML_PARAMETER_VERSION, version),
};

/* The field of name in fields, or NULL. */
typedef struct sxml_parameter_fields {
    const sxml_parameter_field *fields;
    size_t field_count;
} sxml_parameter_fields;

static const sxml_parameter_field *sxml_parameter_find_field(sxml_parameter_fields fields,
                                                             sxml_parameter_name name) {
    for (size_t field_index = 0; field_index < fields.field_count; field_index += 1) {
        if (fields.fields[field_index].name == name) {
            return &fields.fields[field_index];
        }
    }
    return NULL;
}

/* Stores one decoded value; an absent value keeps the default (F&O 4.0,
   17.2.3: () means the default). */
static sxml_status sxml_parameter_assign(sxml_parameter_reading *reading,
                                         sxml_parameter_name name,
                                         const sxml_parameter_value *value) {
    sxml_serialization_parameters *parameters = &reading->parameters;
    if (value->is_absent) {
        return SXML_STATUS_OK;
    }
    uint8_t *const base = (uint8_t *)parameters;
    const sxml_parameter_field *flag_field = sxml_parameter_find_field(
        (sxml_parameter_fields){.fields = sxml_flag_fields,
                                .field_count = sizeof(sxml_flag_fields) / sizeof(sxml_flag_fields[0])},
        name);
    if (flag_field != NULL) {
        const sxml_serialization_flag flag = sxml_parameter_flag_of(value);
        memcpy(base + flag_field->field_offset, &flag, sizeof(flag));
        return SXML_STATUS_OK;
    }
    if (name == SXML_PARAMETER_STANDALONE) {
        parameters->standalone = value->flag ? SXML_STANDALONE_YES : SXML_STANDALONE_NO;
        return SXML_STATUS_OK;
    }
    if (name == SXML_PARAMETER_CDATA_SECTION_ELEMENTS) {
        parameters->cdata_section_elements = value->names;
        return SXML_STATUS_OK;
    }
    if (name == SXML_PARAMETER_SUPPRESS_INDENTATION) {
        parameters->suppress_indentation = value->names;
        return SXML_STATUS_OK;
    }
    if (name == SXML_PARAMETER_USE_CHARACTER_MAPS) {
        parameters->use_character_maps = value->character_map;
        return SXML_STATUS_OK;
    }
    if (name == SXML_PARAMETER_METHOD || name == SXML_PARAMETER_JSON_NODE_OUTPUT_METHOD) {
        return sxml_parameter_assign_method(reading, name, value);
    }
    if (name == SXML_PARAMETER_ENCODING) {
        return sxml_parameter_assign_encoding(reading, sxml_parameter_trim(value->text));
    }
    if (name == SXML_PARAMETER_NORMALIZATION_FORM) {
        return sxml_parameter_assign_normalization(reading, value->text);
    }
    if (name == SXML_PARAMETER_LINE_ENDING) {
        return sxml_parameter_assign_line_ending(reading, value->text);
    }
    if (name == SXML_PARAMETER_INDENT_UNIT) {
        return sxml_parameter_assign_indent_unit(reading, value->text);
    }
    if (name == SXML_PARAMETER_CSV_SEPARATOR || name == SXML_PARAMETER_CSV_QUOTE_CHARACTER) {
        return sxml_parameter_assign_codepoint(reading, name, value->text);
    }
    const sxml_parameter_field *text_field = sxml_parameter_find_field(
        (sxml_parameter_fields){.fields = sxml_text_fields,
                                .field_count = sizeof(sxml_text_fields) / sizeof(sxml_text_fields[0])},
        name);
    if (text_field == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* F&O 4.0, 17.2.3: an empty doctype-public or doctype-system is
       absent. */
    if ((name == SXML_PARAMETER_DOCTYPE_PUBLIC || name == SXML_PARAMETER_DOCTYPE_SYSTEM) &&
        value->text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_byte_view copy = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_parameter_copy_text(reading, value->text, &copy);
    if (status == SXML_STATUS_OK) {
        const bool presence = true;
        memcpy(base + text_field->field_offset, &copy, sizeof(copy));
        memcpy(base + text_field->presence_offset, &presence, sizeof(presence));
    }
    return status;
}
/* ------------------------------------------------------------ The map form */

/* The text of a string-like atomic item (xs:string, xs:anyURI by
   promotion, xs:untypedAtomic by coercion), or false. */
static bool sxml_parameter_item_text(sxml_item item, sxml_byte_view *text) {
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_ATOMIC) {
        return false;
    }
    const uint32_t primitive = sxml_atomic_item_get_primitive_type(item).index;
    if (primitive != SXML_TYPE_XS_STRING && primitive != SXML_TYPE_XS_ANY_URI &&
        primitive != SXML_TYPE_XS_UNTYPED_ATOMIC) {
        return false;
    }
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK) {
        return false;
    }
    *text = datum.datum_details.string.utf8;
    return true;
}

/* A lexical xs:boolean read (XSD 1.1, 3.3.2): true, false, 1, 0. */
typedef struct sxml_boolean_reading {
    bool is_valid;
    bool flag;
} sxml_boolean_reading;

static sxml_boolean_reading sxml_parameter_read_boolean(sxml_byte_view text) {
    const sxml_byte_view trimmed = sxml_parameter_trim(text);
    if (sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
        sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("1"))) {
        return (sxml_boolean_reading){.is_valid = true, .flag = true};
    }
    const bool is_false = sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("false")) ||
                          sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("0"));
    return (sxml_boolean_reading){.is_valid = is_false, .flag = false};
}

/* The lexical form of an xs:decimal: optional sign, digits, at most one
   point. */
static bool sxml_parameter_is_decimal(sxml_byte_view text) {
    size_t offset = 0;
    if (offset < text.byte_count && (text.bytes[0] == '+' || text.bytes[0] == '-')) {
        offset += 1;
    }
    size_t digit_count = 0;
    bool has_point = false;
    for (; offset < text.byte_count; offset += 1) {
        if (text.bytes[offset] >= '0' && text.bytes[offset] <= '9') {
            digit_count += 1;
        } else if (text.bytes[offset] == '.' && !has_point) {
            has_point = true;
        } else {
            return false;
        }
    }
    return digit_count > 0;
}

/* Collects the xs:QName items of a sequence, arrays flattened (the
   coercion of xs:QName* atomizes), into the reading arena. */
typedef struct sxml_name_collection {
    sxml_sequence sequence;
    sxml_element_name_view *names;
} sxml_name_collection;

static sxml_status sxml_parameter_collect_names(sxml_parameter_reading *reading,
                                                const sxml_name_collection *collection) {
    /* Two passes: count, then fill; a stack of arrays being flattened
       would need memory, so arrays nest only one level here (members of
       arrays in the sequence). */
    sxml_name_identifier *names = NULL;
    size_t name_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (int pass_number = 0; status == SXML_STATUS_OK && pass_number < 2; pass_number += 1) {
        if (pass_number == 1 && name_count > 0) {
            void *memory = NULL;
            size_t byte_count = 0;
            status = sxml_size_multiply(name_count, sizeof(sxml_name_identifier), &byte_count);
            if (status == SXML_STATUS_OK) {
                status = sxml_arena_allocate(
                    reading->arena,
                    (sxml_allocation_request){.byte_count = byte_count,
                                              .alignment = alignof(sxml_name_identifier)},
                    &memory);
            }
            names = memory;
        }
        size_t filled = 0;
        uint64_t item_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(collection->sequence, reading->error_record, &item_count);
        }
        for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
             item_index += 1) {
            sxml_item item = {.representation = 0};
            status = sxml_sequence_get_item(
                collection->sequence,
                (sxml_item_request){.item_index = item_index, .error_record = reading->error_record},
                &item);
            size_t member_count = 1;
            const bool is_array =
                status == SXML_STATUS_OK && sxml_item_get_kind(item) == SXML_ITEM_KIND_ARRAY;
            if (is_array) {
                status = sxml_array_get_size(item, &member_count);
            }
            for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
                 member_index += 1) {
                sxml_item name_item = item;
                if (is_array) {
                    sxml_sequence member = sxml_sequence_empty();
                    uint64_t member_item_count = 0;
                    status = sxml_array_get_member(item, member_index, &member);
                    if (status == SXML_STATUS_OK) {
                        status = sxml_sequence_count(member, reading->error_record,
                                                     &member_item_count);
                    }
                    if (status == SXML_STATUS_OK && member_item_count != 1) {
                        return sxml_parameter_raise_type(reading);
                    }
                    if (status == SXML_STATUS_OK) {
                        status = sxml_sequence_get_item(
                            member,
                            (sxml_item_request){.item_index = 0,
                                                .error_record = reading->error_record},
                            &name_item);
                    }
                }
                if (status != SXML_STATUS_OK) {
                    break;
                }
                if (sxml_item_get_kind(name_item) != SXML_ITEM_KIND_ATOMIC ||
                    sxml_atomic_item_get_primitive_type(name_item).index != SXML_TYPE_XS_QNAME) {
                    return sxml_parameter_raise_type(reading);
                }
                sxml_atomic_datum datum;
                status = sxml_atomic_item_get_datum(name_item, &datum);
                if (status == SXML_STATUS_OK && pass_number == 1 && names != NULL &&
                    filled < name_count) {
                    names[filled] = datum.datum_details.qname.expanded_name;
                }
                filled += 1;
            }
        }
        name_count = filled;
    }
    if (status == SXML_STATUS_OK) {
        *collection->names =
            (sxml_element_name_view){.element_names = names, .element_name_count = name_count};
    }
    return status;
}

/* Reads use-character-maps from a map (F&O 4.0, 17.2.3, note 3): keys
   strings of one character (SEPM0016 otherwise), values strings. */
static sxml_status sxml_parameter_read_character_map(sxml_parameter_reading *reading,
                                                     sxml_item map,
                                                     sxml_character_map_view *character_map) {
    size_t size = 0;
    sxml_status status = sxml_map_get_size(map, &size);
    void *memory = NULL;
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK && size > 0) {
        status = sxml_size_multiply(size, sizeof(sxml_character_mapping), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_arena_allocate(
                reading->arena,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_character_mapping)},
                &memory);
        }
    }
    sxml_character_mapping *mappings = memory;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < size;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        sxml_byte_view key = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK && !sxml_parameter_item_text(entry.key, &key)) {
            return sxml_parameter_raise_type(reading);
        }
        const sxml_character_count characters = sxml_parameter_count_characters(key);
        if (status == SXML_STATUS_OK && characters.count != 1) {
            return sxml_parameter_raise_invalid(
                reading, (sxml_invalid_value){.description = "a key of use-character-maps must "
                                                             "be one character",
                                              .is_schema_invalid = false});
        }
        uint64_t value_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(entry.value, reading->error_record, &value_count);
        }
        if (status == SXML_STATUS_OK && value_count != 1) {
            return sxml_parameter_raise_type(reading);
        }
        sxml_item value_item = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                entry.value,
                (sxml_item_request){.item_index = 0, .error_record = reading->error_record},
                &value_item);
        }
        sxml_byte_view replacement = {.bytes = NULL, .byte_count = 0};
        sxml_item string_value = {.representation = 0};
        if (status == SXML_STATUS_OK && sxml_item_get_kind(value_item) == SXML_ITEM_KIND_NODE) {
            /* XPath 4.0, 3.4.3: coercion atomizes a node. */
            sxml_node_reference node;
            status = sxml_item_get_node(value_item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_node_make_string_value(reading->reader->item_heap, node,
                                                     &string_value);
            }
            value_item = string_value;
        }
        if (status == SXML_STATUS_OK && !sxml_parameter_item_text(value_item, &replacement)) {
            status = sxml_parameter_raise_type(reading);
        }
        if (status == SXML_STATUS_OK) {
            mappings[entry_index].codepoint = characters.first_codepoint;
            status = sxml_parameter_copy_text(reading, replacement,
                                              &mappings[entry_index].replacement_text);
        }
        if (string_value.representation != 0) {
            sxml_item_release(string_value);
        }
    }
    if (status == SXML_STATUS_OK) {
        *character_map = (sxml_character_map_view){.character_mappings = mappings,
                                                   .character_mapping_count = size};
    }
    return status;
}

/* Decodes the value of a map entry for one parameter (F&O 4.0, 17.2.3,
   the table of required types). */
typedef struct sxml_map_option {
    sxml_parameter_name name;
    sxml_sequence value;
} sxml_map_option;

static sxml_status sxml_parameter_decode_option(sxml_parameter_reading *reading,
                                                const sxml_map_option *option,
                                                sxml_parameter_value *value) {
    const sxml_parameter_kind kind = sxml_parameter_definitions[option->name].kind;
    if (kind == SXML_PARAMETER_KIND_NAMES) {
        return sxml_parameter_collect_names(
            reading, &(sxml_name_collection){.sequence = option->value, .names = &value->names});
    }
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(option->value, reading->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count == 0) {
        value->is_absent = true;
        return SXML_STATUS_OK;
    }
    if (item_count > 1) {
        return sxml_parameter_raise_type(reading);
    }
    sxml_item item = {.representation = 0};
    status = sxml_sequence_get_item(
        option->value, (sxml_item_request){.item_index = 0, .error_record = reading->error_record},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_item_kind item_kind = sxml_item_get_kind(item);
    const uint32_t primitive = item_kind == SXML_ITEM_KIND_ATOMIC
                                   ? sxml_atomic_item_get_primitive_type(item).index
                                   : 0;
    sxml_atomic_datum datum;
    if (item_kind == SXML_ITEM_KIND_ATOMIC) {
        status = sxml_atomic_item_get_datum(item, &datum);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    switch (kind) {
    case SXML_PARAMETER_KIND_FLAG:
    case SXML_PARAMETER_KIND_STANDALONE:
        if (primitive == SXML_TYPE_XS_BOOLEAN) {
            value->flag = datum.datum_details.boolean;
            return SXML_STATUS_OK;
        }
        if (primitive == SXML_TYPE_XS_UNTYPED_ATOMIC) {
            const sxml_boolean_reading reading_result =
                sxml_parameter_read_boolean(datum.datum_details.string.utf8);
            value->flag = reading_result.flag;
            if (reading_result.is_valid) {
                return SXML_STATUS_OK;
            }
        }
        return sxml_parameter_raise_type(reading);
    case SXML_PARAMETER_KIND_TEXT:
        return sxml_parameter_item_text(item, &value->text) ? SXML_STATUS_OK
                                                            : sxml_parameter_raise_type(reading);
    case SXML_PARAMETER_KIND_METHOD:
        if (primitive == SXML_TYPE_XS_QNAME) {
            value->is_qualified_method = true;
            return SXML_STATUS_OK;
        }
        return sxml_parameter_item_text(item, &value->text) ? SXML_STATUS_OK
                                                            : sxml_parameter_raise_type(reading);
    case SXML_PARAMETER_KIND_DECIMAL:
        if (primitive == SXML_TYPE_XS_DECIMAL || primitive == SXML_TYPE_XS_UNTYPED_ATOMIC) {
            sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
            uint8_t digits[32];
            if (primitive == SXML_TYPE_XS_UNTYPED_ATOMIC) {
                text = sxml_parameter_trim(datum.datum_details.string.utf8);
                if (!sxml_parameter_is_decimal(text)) {
                    return sxml_parameter_raise_type(reading);
                }
            } else if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
                int64_t integer = datum.datum_details.integer;
                const bool is_negative = integer < 0;
                uint64_t magnitude = is_negative ? 0U - (uint64_t)integer : (uint64_t)integer;
                size_t position = sizeof(digits);
                do {
                    digits[--position] = (uint8_t)('0' + magnitude % 10U);
                    magnitude /= 10U;
                } while (magnitude != 0);
                if (is_negative) {
                    digits[--position] = '-';
                }
                text = (sxml_byte_view){.bytes = digits + position,
                                        .byte_count = sizeof(digits) - position};
            } else {
                text = datum.datum_details.decimal_text;
            }
            return sxml_parameter_copy_text(reading, text, &value->text);
        }
        return sxml_parameter_raise_type(reading);
    case SXML_PARAMETER_KIND_CHARACTER_MAP:
        if (item_kind != SXML_ITEM_KIND_MAP) {
            return sxml_parameter_raise_type(reading);
        }
        return sxml_parameter_read_character_map(reading, item, &value->character_map);
    case SXML_PARAMETER_KIND_NAMES:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Reads the options map (F&O 4.0, 17.2.3, and 1.7 "Options"): string keys
   name parameters (XPTY0004 for a name that is none), xs:QName keys in a
   namespace are implementation-defined parameters and ignored, an
   xs:QName in no namespace is XPTY0004. */
static sxml_status sxml_parameter_read_map(sxml_parameter_reading *reading, sxml_item map) {
    size_t size = 0;
    sxml_status status = sxml_map_get_size(map, &size);
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(reading->reader->item_heap);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < size;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_byte_view key = {.bytes = NULL, .byte_count = 0};
        if (sxml_item_get_kind(entry.key) == SXML_ITEM_KIND_ATOMIC &&
            sxml_atomic_item_get_primitive_type(entry.key).index == SXML_TYPE_XS_QNAME) {
            sxml_atomic_datum datum;
            sxml_name_parts parts;
            status = sxml_atomic_item_get_datum(entry.key, &datum);
            if (status == SXML_STATUS_OK) {
                status = sxml_name_pool_get_name_parts(
                    name_pool, datum.datum_details.qname.expanded_name, &parts);
            }
            if (status == SXML_STATUS_OK && parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
                status = sxml_parameter_raise_type(reading);
            }
            continue;
        }
        if (!sxml_parameter_item_text(entry.key, &key)) {
            continue;
        }
        const sxml_parameter_name name = sxml_parameter_find(key);
        if (name == SXML_PARAMETER_COUNT) {
            status = sxml_parameter_raise_type(reading);
            break;
        }
        sxml_parameter_value value;
        memset(&value, 0, sizeof(value));
        status = sxml_parameter_decode_option(
            reading, &(sxml_map_option){.name = name, .value = entry.value}, &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_parameter_assign(reading, name, &value);
        }
    }
    return status;
}

/* --------------------------------------------------------- The element form */

/* The codes of a parameter document's errors (Serialization 4.0, 3.1). */
typedef enum sxml_document_error {
    SXML_DOCUMENT_ERROR_SEPM0017,
    SXML_DOCUMENT_ERROR_SEPM0018,
    SXML_DOCUMENT_ERROR_SEPM0019,
} sxml_document_error;

static sxml_status sxml_parameter_raise_document(sxml_parameter_reading *reading,
                                                 sxml_document_error error) {
    static const char *const codes[] = {"SEPM0017", "SEPM0018", "SEPM0019"};
    return sxml_parameter_raise(
        reading, (sxml_parameter_error){.code = codes[error],
                                        .description = "a parameter document does not follow "
                                                       "Serialization 4.0, section 3.1",
                                        .status = SXML_STATUS_DYNAMIC_ERROR});
}

/* A value whose lexical form is not the parameter's: SEPM0017 in a
   parameter document, SEPM0016 in an output declaration (XQuery 4.0,
   5.22). */
static sxml_status sxml_parameter_raise_form(sxml_parameter_reading *reading) {
    return reading->is_document
               ? sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017)
               : sxml_parameter_raise_invalid(
                     reading, (sxml_invalid_value){.description = "a serialization parameter "
                                                                  "has a value it does not permit",
                                                   .is_schema_invalid = false});
}

/* The attributes of an element of a parameter document: only those
   allowed, in no namespace (attributes in other namespaces are ignored,
   lax validation). */
typedef struct sxml_attribute_lookup {
    sxml_node_reference element;
    /* Names of the allowed attributes in no namespace, and where their
       values go (NULL when absent). */
    const char *const *names;
    sxml_byte_view *values;
    bool *presences;
    size_t name_count;
} sxml_attribute_lookup;

static sxml_status sxml_parameter_read_attributes(sxml_parameter_reading *reading,
                                                  const sxml_attribute_lookup *lookup) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(lookup->element, &description);
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(reading->reader->item_heap);
    for (size_t name_index = 0; name_index < lookup->name_count; name_index += 1) {
        lookup->presences[name_index] = false;
    }
    for (uint32_t attribute_number = 1;
         status == SXML_STATUS_OK && attribute_number <= description.attribute_count;
         attribute_number += 1) {
        const sxml_node_reference attribute = {
            .tree = lookup->element.tree,
            .node_index = {.index = lookup->element.node_index.index + attribute_number},
            .namespace_number = 0};
        sxml_node_description attribute_description;
        sxml_name_parts parts;
        status = sxml_node_describe(attribute, &attribute_description);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(name_pool, attribute_description.node_name,
                                                   &parts);
        }
        if (status != SXML_STATUS_OK || parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
            continue;
        }
        bool is_known = false;
        for (size_t name_index = 0; name_index < lookup->name_count; name_index += 1) {
            if (sxml_byte_view_equals(parts.local_name,
                                      sxml_byte_view_from_c_string(lookup->names[name_index]))) {
                is_known = true;
                lookup->presences[name_index] = true;
                status = sxml_node_get_content(attribute, &lookup->values[name_index]);
            }
        }
        if (status == SXML_STATUS_OK && !is_known) {
            return sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
        }
    }
    return status;
}

/* Resolves a lexical QName (prefix:local, local, or Q{uri}local) against
   namespace bindings (an element's in-scope namespaces, or the statically
   known namespaces of a query; the first binding of a prefix holds); an
   unprefixed name takes the default namespace (prefix 0) when
   uses_default_namespace. */
typedef struct sxml_lexical_qname {
    sxml_namespace_binding_view namespace_bindings;
    sxml_byte_view text;
    bool uses_default_namespace;
} sxml_lexical_qname;

static sxml_status sxml_parameter_resolve_qname(sxml_parameter_reading *reading,
                                                const sxml_lexical_qname *qname,
                                                sxml_name_identifier *name) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(reading->reader->item_heap);
    const sxml_byte_view text = qname->text;
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_byte_view local_name = text;
    sxml_status status = SXML_STATUS_OK;
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        size_t close = 2;
        while (close < text.byte_count && text.bytes[close] != '}') {
            close += 1;
        }
        if (close == text.byte_count) {
            return sxml_parameter_raise_form(reading);
        }
        status = sxml_name_pool_intern_namespace_uri(
            name_pool, (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = close - 2},
            &namespace_uri);
        local_name = (sxml_byte_view){.bytes = text.bytes + close + 1,
                                      .byte_count = text.byte_count - close - 1};
    } else {
        size_t colon = 0;
        while (colon < text.byte_count && text.bytes[colon] != ':') {
            colon += 1;
        }
        const bool has_prefix = colon < text.byte_count;
        const sxml_byte_view prefix = {.bytes = text.bytes, .byte_count = has_prefix ? colon : 0};
        if (has_prefix) {
            local_name = (sxml_byte_view){.bytes = text.bytes + colon + 1,
                                          .byte_count = text.byte_count - colon - 1};
        }
        if (has_prefix || qname->uses_default_namespace) {
            const sxml_namespace_binding_view bindings = qname->namespace_bindings;
            bool is_found = false;
            for (size_t binding_index = 0; status == SXML_STATUS_OK && !is_found &&
                                           binding_index < bindings.namespace_binding_count;
                 binding_index += 1) {
                const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
                sxml_byte_view bound_prefix = {.bytes = NULL, .byte_count = 0};
                if (binding.namespace_prefix.index != 0) {
                    status = sxml_name_pool_get_namespace_prefix(name_pool, binding.namespace_prefix,
                                                                 &bound_prefix);
                }
                if (status == SXML_STATUS_OK && sxml_byte_view_equals(bound_prefix, prefix)) {
                    namespace_uri = binding.namespace_uri;
                    is_found = true;
                }
            }
            if (status == SXML_STATUS_OK && !is_found && has_prefix) {
                return sxml_parameter_raise_form(reading);
            }
        }
    }
    if (status == SXML_STATUS_OK && local_name.byte_count == 0) {
        return sxml_parameter_raise_form(reading);
    }
    return status == SXML_STATUS_OK
               ? sxml_name_pool_intern_name(
                     name_pool,
                     &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
                     name)
               : status;
}

/* Reads a whitespace-separated list of QNames (cdata-section-elements,
   suppress-indentation). */
static sxml_status sxml_parameter_read_name_list(sxml_parameter_reading *reading,
                                                 const sxml_lexical_qname *list,
                                                 sxml_element_name_view *names) {
    size_t token_count = 0;
    const sxml_byte_view text = list->text;
    for (size_t offset = 0; offset < text.byte_count; offset += 1) {
        const bool is_space = text.bytes[offset] == ' ' || text.bytes[offset] == '\t' ||
                              text.bytes[offset] == '\n' || text.bytes[offset] == '\r';
        const bool was_space = offset == 0 || text.bytes[offset - 1] == ' ' ||
                               text.bytes[offset - 1] == '\t' || text.bytes[offset - 1] == '\n' ||
                               text.bytes[offset - 1] == '\r';
        token_count += !is_space && was_space ? 1U : 0U;
    }
    void *memory = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (token_count > 0) {
        status = sxml_arena_allocate(
            reading->arena,
            (sxml_allocation_request){.byte_count = token_count * sizeof(sxml_name_identifier),
                                      .alignment = alignof(sxml_name_identifier)},
            &memory);
    }
    sxml_name_identifier *identifiers = memory;
    size_t filled = 0;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        while (offset < text.byte_count &&
               (text.bytes[offset] == ' ' || text.bytes[offset] == '\t' ||
                text.bytes[offset] == '\n' || text.bytes[offset] == '\r')) {
            offset += 1;
        }
        const size_t start = offset;
        while (offset < text.byte_count && text.bytes[offset] != ' ' &&
               text.bytes[offset] != '\t' && text.bytes[offset] != '\n' &&
               text.bytes[offset] != '\r') {
            offset += 1;
        }
        if (offset > start) {
            status = sxml_parameter_resolve_qname(
                reading,
                &(sxml_lexical_qname){
                    .namespace_bindings = list->namespace_bindings,
                    .text = {.bytes = text.bytes + start, .byte_count = offset - start},
                    .uses_default_namespace = list->uses_default_namespace},
                &identifiers[filled]);
            filled += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        *names = (sxml_element_name_view){.element_names = identifiers,
                                          .element_name_count = filled};
    }
    return status;
}

/* Reads use-character-maps from its element: output:character-map
   children with character and map-string (SEPM0017 for anything else,
   SEPM0018 for a character given twice). */
static sxml_status sxml_parameter_read_character_map_element(
    sxml_parameter_reading *reading, sxml_node_reference element,
    sxml_character_map_view *character_map) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(element, &description);
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(reading->reader->item_heap);
    size_t child_count = 0;
    for (int pass_number = 0; status == SXML_STATUS_OK && pass_number < 2; pass_number += 1) {
        sxml_character_mapping *mappings = NULL;
        if (pass_number == 1 && child_count > 0) {
            void *memory = NULL;
            status = sxml_arena_allocate(
                reading->arena,
                (sxml_allocation_request){.byte_count = child_count * sizeof(sxml_character_mapping),
                                          .alignment = alignof(sxml_character_mapping)},
                &memory);
            mappings = memory;
        }
        size_t filled = 0;
        bool has_child = description.has_children;
        sxml_node_index child = description.first_child;
        while (status == SXML_STATUS_OK && has_child) {
            const sxml_node_reference child_node = {
                .tree = element.tree, .node_index = child, .namespace_number = 0};
            sxml_node_description child_description;
            status = sxml_node_describe(child_node, &child_description);
            if (status != SXML_STATUS_OK) {
                break;
            }
            has_child = child_description.has_next_sibling;
            child = child_description.next_sibling;
            if (child_description.node_kind != SXML_NODE_KIND_ELEMENT) {
                continue;
            }
            sxml_name_parts parts;
            status = sxml_name_pool_get_name_parts(name_pool, child_description.node_name, &parts);
            if (status == SXML_STATUS_OK &&
                (parts.namespace_uri.index != SXML_NAMESPACE_SERIALIZATION ||
                 !sxml_byte_view_equals(parts.local_name,
                                        SXML_BYTE_VIEW_FROM_LITERAL("character-map")))) {
                return sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
            }
            if (pass_number == 0) {
                filled += 1;
                continue;
            }
            static const char *const names[] = {"character", "map-string"};
            sxml_byte_view values[2] = {{.bytes = NULL, .byte_count = 0},
                                        {.bytes = NULL, .byte_count = 0}};
            bool presences[2] = {false, false};
            status = sxml_parameter_read_attributes(
                reading, &(sxml_attribute_lookup){.element = child_node,
                                                  .names = names,
                                                  .values = values,
                                                  .presences = presences,
                                                  .name_count = 2});
            const sxml_character_count characters = sxml_parameter_count_characters(values[0]);
            if (status == SXML_STATUS_OK &&
                (!presences[0] || !presences[1] || characters.count != 1)) {
                return sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
            }
            if (mappings == NULL || filled >= child_count) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            for (size_t earlier = 0; status == SXML_STATUS_OK && earlier < filled; earlier += 1) {
                if (mappings[earlier].codepoint == characters.first_codepoint) {
                    return sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0018);
                }
            }
            if (status == SXML_STATUS_OK) {
                mappings[filled].codepoint = characters.first_codepoint;
                status = sxml_parameter_copy_text(reading, values[1],
                                                  &mappings[filled].replacement_text);
                filled += 1;
            }
        }
        child_count = filled;
        if (pass_number == 1 && status == SXML_STATUS_OK) {
            *character_map = (sxml_character_map_view){.character_mappings = mappings,
                                                       .character_mapping_count = filled};
        }
    }
    return status;
}

/* Decodes a parameter's value given as text, the lexical form of a
   parameter document (Serialization 4.0, appendix B: yes-no types, QName
   lists, decimals): the value attribute of a parameter element, or an
   XQuery output declaration (XQuery 4.0, 5.22). QNames resolve with
   namespace_bindings. */
typedef struct sxml_parameter_text_decoding {
    sxml_parameter_name name;
    sxml_byte_view text;
    sxml_namespace_binding_view namespace_bindings;
} sxml_parameter_text_decoding;

static sxml_status sxml_parameter_decode_text(sxml_parameter_reading *reading,
                                              const sxml_parameter_text_decoding *decoding,
                                              sxml_parameter_value *value) {
    const sxml_parameter_kind kind = sxml_parameter_definitions[decoding->name].kind;
    const sxml_byte_view text = decoding->text;
    const sxml_byte_view trimmed = sxml_parameter_trim(text);
    switch (kind) {
    case SXML_PARAMETER_KIND_FLAG:
    case SXML_PARAMETER_KIND_STANDALONE: {
        if (sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("yes"))) {
            value->flag = true;
            return SXML_STATUS_OK;
        }
        if (sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("no"))) {
            value->flag = false;
            return SXML_STATUS_OK;
        }
        if (kind == SXML_PARAMETER_KIND_STANDALONE &&
            sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("omit"))) {
            value->is_absent = true;
            return SXML_STATUS_OK;
        }
        const sxml_boolean_reading reading_result = sxml_parameter_read_boolean(trimmed);
        value->flag = reading_result.flag;
        return reading_result.is_valid ? SXML_STATUS_OK : sxml_parameter_raise_form(reading);
    }
    case SXML_PARAMETER_KIND_TEXT:
        value->text = text;
        return SXML_STATUS_OK;
    case SXML_PARAMETER_KIND_DECIMAL:
        if (!sxml_parameter_is_decimal(trimmed)) {
            return sxml_parameter_raise_form(reading);
        }
        value->text = trimmed;
        return SXML_STATUS_OK;
    case SXML_PARAMETER_KIND_METHOD: {
        sxml_serialization_method method = SXML_SERIALIZATION_METHOD_XML;
        if (sxml_parameter_find_method(trimmed, &method)) {
            value->text = trimmed;
            return SXML_STATUS_OK;
        }
        /* Otherwise a QName with a prefix: an implementation-defined
           method (appendix B, method-type). */
        bool has_colon = false;
        for (size_t offset = 0; offset < trimmed.byte_count; offset += 1) {
            has_colon = has_colon || trimmed.bytes[offset] == ':';
        }
        if (!has_colon) {
            return sxml_parameter_raise_form(reading);
        }
        sxml_name_identifier unused_name;
        const sxml_status status = sxml_parameter_resolve_qname(
            reading,
            &(sxml_lexical_qname){.namespace_bindings = decoding->namespace_bindings,
                                  .text = trimmed,
                                  .uses_default_namespace = false},
            &unused_name);
        value->is_qualified_method = true;
        return status;
    }
    case SXML_PARAMETER_KIND_NAMES:
        return sxml_parameter_read_name_list(
            reading,
            &(sxml_lexical_qname){.namespace_bindings = decoding->namespace_bindings,
                                  .text = text,
                                  .uses_default_namespace = true},
            &value->names);
    case SXML_PARAMETER_KIND_CHARACTER_MAP:
        /* Only as an element (use-character-maps has no lexical form). */
        return sxml_parameter_raise_form(reading);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Decodes a parameter element: its value attribute, or for
   use-character-maps its output:character-map children. */
typedef struct sxml_parameter_element {
    sxml_parameter_name name;
    sxml_node_reference element;
} sxml_parameter_element;

static sxml_status sxml_parameter_decode_element(sxml_parameter_reading *reading,
                                                 const sxml_parameter_element *parameter,
                                                 sxml_parameter_value *value) {
    static const char *const names[] = {"value"};
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    bool has_value = false;
    sxml_status status = sxml_parameter_read_attributes(
        reading, &(sxml_attribute_lookup){.element = parameter->element,
                                          .names = names,
                                          .values = &text,
                                          .presences = &has_value,
                                          .name_count = 1});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_parameter_definitions[parameter->name].kind == SXML_PARAMETER_KIND_CHARACTER_MAP) {
        return has_value ? sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017)
                         : sxml_parameter_read_character_map_element(reading, parameter->element,
                                                                     &value->character_map);
    }
    if (!has_value) {
        return sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    status = sxml_node_get_in_scope_namespaces(parameter->element, &bindings);
    return status == SXML_STATUS_OK
               ? sxml_parameter_decode_text(
                     reading,
                     &(sxml_parameter_text_decoding){
                         .name = parameter->name, .text = text, .namespace_bindings = bindings},
                     value)
               : status;
}

/* Reads a parameter document (Serialization 4.0, 3.1). */
static sxml_status sxml_parameter_read_document(sxml_parameter_reading *reading,
                                                sxml_node_reference element) {
    reading->is_document = true;
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(reading->reader->item_heap);
    sxml_node_description description;
    sxml_status status = sxml_node_describe(element, &description);
    /* Attributes of the element itself: none in no namespace. */
    if (status == SXML_STATUS_OK) {
        status = sxml_parameter_read_attributes(
            reading, &(sxml_attribute_lookup){.element = element,
                                              .names = NULL,
                                              .values = NULL,
                                              .presences = NULL,
                                              .name_count = 0});
    }
    bool has_child = description.has_children;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child) {
        const sxml_node_reference child_node = {
            .tree = element.tree, .node_index = child, .namespace_number = 0};
        sxml_node_description child_description;
        status = sxml_node_describe(child_node, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
        if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(child_node, &content);
            if (status == SXML_STATUS_OK && sxml_parameter_trim(content).byte_count > 0) {
                status = sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
            }
            continue;
        }
        if (child_description.node_kind != SXML_NODE_KIND_ELEMENT) {
            continue;
        }
        /* SEPM0019: no two children with the same name, whatever their
           namespace. */
        bool has_earlier_twin = false;
        bool has_earlier = child_description.has_previous_sibling;
        sxml_node_index earlier = child_description.previous_sibling;
        while (status == SXML_STATUS_OK && has_earlier && !has_earlier_twin) {
            sxml_node_description earlier_description;
            status = sxml_node_describe(
                (sxml_node_reference){.tree = element.tree, .node_index = earlier,
                                      .namespace_number = 0},
                &earlier_description);
            has_earlier_twin = status == SXML_STATUS_OK &&
                               earlier_description.node_kind == SXML_NODE_KIND_ELEMENT &&
                               earlier_description.node_name.index ==
                                   child_description.node_name.index;
            has_earlier = earlier_description.has_previous_sibling;
            earlier = earlier_description.previous_sibling;
        }
        if (status == SXML_STATUS_OK && has_earlier_twin) {
            status = sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0019);
            break;
        }
        sxml_name_parts parts;
        status = sxml_name_pool_get_name_parts(name_pool, child_description.node_name, &parts);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
            status = sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
            break;
        }
        if (parts.namespace_uri.index != SXML_NAMESPACE_SERIALIZATION) {
            continue;
        }
        const sxml_parameter_name name = sxml_parameter_find(parts.local_name);
        if (name == SXML_PARAMETER_COUNT) {
            status = sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
            break;
        }
        sxml_parameter_value value;
        memset(&value, 0, sizeof(value));
        status = sxml_parameter_decode_element(
            reading, &(sxml_parameter_element){.name = name, .element = child_node}, &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_parameter_assign(reading, name, &value);
        }
    }
    return status;
}

/* ------------------------------------------------------------- Reading */

static sxml_status sxml_parameter_reader_prepare(sxml_serialization_parameter_reader *reader) {
    const sxml_arena_configuration configuration = {
        .block_allocator = reader->allocator,
        .block_byte_count = 1024,
        .maximum_block_byte_count = 0,
        .initial_memory = {.bytes = NULL, .byte_count = 0},
    };
    sxml_status status = SXML_STATUS_OK;
    if (reader->current_arena == NULL) {
        status = sxml_arena_create(reader->allocator, &configuration, &reader->current_arena);
    }
    if (status == SXML_STATUS_OK && reader->spare_arena == NULL) {
        status = sxml_arena_create(reader->allocator, &configuration, &reader->spare_arena);
    }
    return status;
}

/* Makes the arena a successful read filled current, so the last read's
   parameters stay valid until the next success. */
static void sxml_parameter_reader_commit(sxml_serialization_parameter_reader *reader) {
    sxml_arena *const previous = reader->current_arena;
    reader->current_arena = reader->spare_arena;
    reader->spare_arena = previous;
    sxml_arena_reset(previous);
}

sxml_status sxml_serialization_parameter_reader_read(
    sxml_serialization_parameter_reader *parameter_reader,
    const sxml_serialization_parameter_read_request *request,
    sxml_serialization_parameters *parameters) {
    if (parameter_reader == NULL || request == NULL || parameters == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_parameter_reader_prepare(parameter_reader);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_arena_reset(parameter_reader->spare_arena);
    /* F&O 4.0, 17.2.3: method xml when the options give none. */
    sxml_parameter_reading reading = {
        .reader = parameter_reader,
        .arena = parameter_reader->spare_arena,
        .error_record = request->error_record,
        .parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_XML},
        .is_document = false,
    };
    uint64_t item_count = 0;
    status = sxml_sequence_count(request->options, request->error_record, &item_count);
    sxml_item options = {.representation = 0};
    if (status == SXML_STATUS_OK && item_count > 1) {
        status = sxml_parameter_raise_type(&reading);
    }
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = sxml_sequence_get_item(
            request->options,
            (sxml_item_request){.item_index = 0, .error_record = request->error_record}, &options);
    }
    if (status == SXML_STATUS_OK && item_count == 1) {
        const sxml_item_kind kind = sxml_item_get_kind(options);
        sxml_node_reference node;
        sxml_node_description description;
        sxml_name_parts parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        if (kind == SXML_ITEM_KIND_MAP) {
            status = sxml_parameter_read_map(&reading, options);
        } else if (kind == SXML_ITEM_KIND_NODE) {
            status = sxml_item_get_node(options, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_node_describe(node, &description);
            }
            if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
                status = sxml_name_pool_get_name_parts(
                    sxml_item_heap_get_name_pool(parameter_reader->item_heap),
                    description.node_name, &parts);
            }
            const bool is_parameter_document =
                status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT &&
                parts.namespace_uri.index == SXML_NAMESPACE_SERIALIZATION &&
                sxml_byte_view_equals(parts.local_name,
                                      SXML_BYTE_VIEW_FROM_LITERAL("serialization-parameters"));
            if (status == SXML_STATUS_OK) {
                status = is_parameter_document ? sxml_parameter_read_document(&reading, node)
                                               : sxml_parameter_raise_type(&reading);
            }
        } else {
            status = sxml_parameter_raise_type(&reading);
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_reset(parameter_reader->spare_arena);
        return status;
    }
    sxml_parameter_reader_commit(parameter_reader);
    *parameters = reading.parameters;
    return SXML_STATUS_OK;
}

/* Reads the parameter document, then the texts over it. */
static sxml_status sxml_parameter_read_texts(sxml_parameter_reading *reading,
                                             const sxml_serialization_parameter_text_request *request) {
    sxml_status status = SXML_STATUS_OK;
    if (request->has_parameter_document) {
        const sxml_name_pool *name_pool =
            sxml_item_heap_get_name_pool(reading->reader->item_heap);
        sxml_node_description description;
        sxml_name_parts parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_node_describe(request->parameter_document, &description);
        if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
            status = sxml_name_pool_get_name_parts(name_pool, description.node_name, &parts);
        }
        reading->is_document = true;
        if (status == SXML_STATUS_OK) {
            /* Serialization 4.0, 3.1: the document element is
               output:serialization-parameters. */
            status = description.node_kind == SXML_NODE_KIND_ELEMENT &&
                             parts.namespace_uri.index == SXML_NAMESPACE_SERIALIZATION &&
                             sxml_byte_view_equals(
                                 parts.local_name,
                                 SXML_BYTE_VIEW_FROM_LITERAL("serialization-parameters"))
                         ? sxml_parameter_read_document(reading, request->parameter_document)
                         : sxml_parameter_raise_document(reading, SXML_DOCUMENT_ERROR_SEPM0017);
        }
        reading->is_document = false;
    }
    const sxml_serialization_parameter_text_view texts = request->parameter_texts;
    for (size_t text_index = 0; status == SXML_STATUS_OK && text_index < texts.parameter_text_count;
         text_index += 1) {
        const sxml_serialization_parameter_text *text = &texts.parameter_texts[text_index];
        const sxml_parameter_name name = sxml_parameter_find(text->parameter_name);
        sxml_parameter_value value;
        memset(&value, 0, sizeof(value));
        status = name == SXML_PARAMETER_COUNT
                     ? sxml_parameter_raise_form(reading)
                     : sxml_parameter_decode_text(
                           reading,
                           &(sxml_parameter_text_decoding){
                               .name = name,
                               .text = text->parameter_value,
                               .namespace_bindings = request->namespace_bindings},
                           &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_parameter_assign(reading, name, &value);
        }
        if (status != SXML_STATUS_OK && request->failed_text_index != NULL) {
            *request->failed_text_index = text_index;
        }
    }
    return status;
}

sxml_status sxml_serialization_parameter_reader_read_texts(
    sxml_serialization_parameter_reader *parameter_reader,
    const sxml_serialization_parameter_text_request *request,
    sxml_serialization_parameters *parameters) {
    if (parameter_reader == NULL || request == NULL || parameters == NULL ||
        (request->parameter_texts.parameter_text_count > 0 &&
         request->parameter_texts.parameter_texts == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->failed_text_index != NULL) {
        *request->failed_text_index = SIZE_MAX;
    }
    sxml_status status = sxml_parameter_reader_prepare(parameter_reader);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_arena_reset(parameter_reader->spare_arena);
    /* XQuery 4.0, 5.22.1, and XSLT's xsl:output: method xml when nothing
       gives one. */
    sxml_parameter_reading reading = {
        .reader = parameter_reader,
        .arena = parameter_reader->spare_arena,
        .error_record = request->error_record,
        .parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_XML},
        .is_document = false,
    };
    status = sxml_parameter_read_texts(&reading, request);
    if (status != SXML_STATUS_OK) {
        sxml_arena_reset(parameter_reader->spare_arena);
        return status;
    }
    sxml_parameter_reader_commit(parameter_reader);
    *parameters = reading.parameters;
    return SXML_STATUS_OK;
}
