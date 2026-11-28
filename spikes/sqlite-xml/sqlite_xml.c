/*
 * A spike: sxml as a SQLite extension (README.md).
 *
 *   xmlb(doc)               XML text (or XDMB) -> XDMB blob
 *   xml(doc)                XDMB (or text) -> XML text
 *   xml_xpath(doc, expr)    XPath 4.0; one atomic item comes back as an
 *                           SQL integer, real or text, anything else
 *                           serialized (adaptive method); empty: NULL
 *   xml_exists(doc, expr)   boolean(expr) as 0 or 1, for WHERE
 *   xml_query(doc, query)   XQuery 4.0, serialized as the query declares
 *
 * doc is XML text or an XDMB blob; a blob is queried in place. Compiled
 * expressions are cached per statement with sqlite3_set_auxdata, so a
 * constant expression compiles once per statement, not once per row.
 * One sxml processor per connection.
 */
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <string.h>

#include "sxml_host.h"
#include "sxml_host_internal.h"
#include "xdmb.h"

typedef struct sqlite_xml_connection {
    sxml_host_processor *processor;
    /* xml_exi_schema: the schema and alignment of EXI streams whose header
       carries no options (schema-informed streams are stored without). */
    sxml_host_schema *exi_schema;
    uint32_t exi_alignment;
    /* xml_schema(xsd): the schema xmlb(doc, 'validate') validates with,
       and typed XDMB blobs are opened with. */
    sxml_host_schema *schema;
    /* What the last xmlb wrote as an approximation (xmlb_approximate()). */
    size_t approximate_type_count;
    /* The operators (sqlite3_xml_operators_init): a private in-memory
       connection with SQLite's own -> and ->>, which answers for every
       left operand that is not XML, and its two statements. */
    sqlite3 *json;
    sqlite3_stmt *json_arrow;
    sqlite3_stmt *json_double_arrow;
} sqlite_xml_connection;

static void sqlite_xml_fail(sqlite3_context *context, sxml_host_processor *processor) {
    const sxml_host_error *error = sxml_host_error_of(processor);
    char *message = sqlite3_mprintf("sxml: %.*s %.*s: %.*s", (int)error->code_namespace_prefix.byte_count,
                                    error->code_namespace_prefix.bytes, (int)error->code_local_name.byte_count,
                                    error->code_local_name.bytes, (int)error->description.byte_count,
                                    error->description.bytes);
    if (error->description.byte_count == 0) {
        sqlite3_free(message);
        message = sqlite3_mprintf("sxml: %.*s", (int)error->status_name.byte_count, error->status_name.bytes);
    }
    sqlite3_result_error(context, message, -1);
    sqlite3_free(message);
}

/* Whether bytes are an EXI stream: the cookie "$EXI", or the
   distinguishing bits 10 (EXI 1.0, 5.1 and 5.2). XDMB starts with 'X'
   (01011000) and XML text with '<' or white space, so neither is one. */
static int sqlite_xml_is_exi(sxml_byte_view bytes) {
    if (bytes.byte_count >= 4 && memcmp(bytes.bytes, "$EXI", 4) == 0) {
        return 1;
    }
    return bytes.byte_count > 0 && (bytes.bytes[0] & 0xC0U) == 0x80U;
}

/* The document node of argument, as a value: an XDMB blob opened in place
   (valid for this call: SQLite keeps the bytes until the function
   returns), else XML text parsed. NULL with the error set. */
static sxml_host_value *sqlite_xml_document(sqlite3_context *context, sxml_host_processor *processor,
                                            sqlite3_value *argument) {
    sxml_byte_view bytes = {sqlite3_value_blob(argument), (size_t)sqlite3_value_bytes(argument)};
    if (sqlite3_value_type(argument) == SQLITE_BLOB && xdmb_is_encoded(bytes)) {
        sxml_tree *tree = NULL;
        sxml_status status = xdmb_open(
            &(xdmb_open_request){.bytes = bytes,
                                 .item_heap = sxml_processor_get_item_heap(processor->processor),
                                 .allocator = processor->allocator,
                                 .type_registry = sxml_processor_get_type_registry(processor->processor),
                                 .schema = ((sqlite_xml_connection *)sqlite3_user_data(context))->schema
                                               ? sxml_host_schema_compiled(
                                                     ((sqlite_xml_connection *)sqlite3_user_data(context))->schema)
                                               : NULL},
            &tree);
        if (status != SXML_STATUS_OK) {
            sqlite3_result_error(context, "sxml: not a valid XDMB blob", -1);
            return NULL;
        }
        sxml_item item = sxml_node_item_make((sxml_node_reference){.tree = tree, .node_index = {0}});
        sxml_tree_release(tree); /* the item holds it now */
        sxml_host_value *value = sxml_host_value_make(processor, sxml_sequence_from_item(item));
        if (value == NULL) {
            sqlite3_result_error_nomem(context);
        }
        return value;
    }
    if (sqlite3_value_type(argument) == SQLITE_BLOB && sqlite_xml_is_exi(bytes)) {
        /* EXI (EXI 1.0, 5): decoded into a tree for each call, its options
           read from its header. */
        sqlite_xml_connection *connection = sqlite3_user_data(context);
        sxml_host_exi_parse_request request = {.input = bytes, .document_uri = {0}, .options = {0}};
        size_t header = bytes.byte_count >= 4 && memcmp(bytes.bytes, "$EXI", 4) == 0 ? 4 : 0;
        int has_options = header < bytes.byte_count && (bytes.bytes[header] & 0x20U) != 0;
        if (!has_options) {
            /* no options in the header: the connection's (xml_exi_schema) */
            request.options.schema = connection->exi_schema;
            request.options.alignment = connection->exi_alignment;
        }
        sxml_host_value *value = sxml_host_exi_parse(processor, &request);
        if (value == NULL) {
            sqlite_xml_fail(context, processor);
        }
        return value;
    }
    bytes.bytes = sqlite3_value_text(argument);
    bytes.byte_count = (size_t)sqlite3_value_bytes(argument);
    sxml_host_value *value =
        sxml_host_parse(processor, &(sxml_host_parse_request){.input = bytes, .document_uri = {0}});
    if (value == NULL) {
        sqlite_xml_fail(context, processor);
    }
    return value;
}

/* ------------------------------------------------- xmlb(doc), xml(doc) */

/* xmlb(doc [, options]): options a text naming 'validate' (validate with
   the connection's schema, xml_schema, and keep the type annotations) and
   'locations' (keep the nodes' source locations). */
static void sqlite_xml_xmlb(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    if (sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
        return;
    }
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    const char *options = argument_count > 1 ? (const char *)sqlite3_value_text(arguments[1]) : NULL;
    int validates = options != NULL && strstr(options, "validate") != NULL;
    int keeps_locations = options != NULL && strstr(options, "locations") != NULL;
    sxml_host_value *document = sqlite_xml_document(context, connection->processor, arguments[0]);
    if (document == NULL) {
        return;
    }
    if (validates) {
        if (connection->schema == NULL) {
            sqlite3_result_error(context, "sxml: xmlb(doc, 'validate') needs xml_schema(xsd) first", -1);
            sxml_host_value_release(document);
            return;
        }
        sxml_host_validate_request validation = {.node = document};
        sxml_host_value *validated = sxml_host_schema_validate(connection->schema, &validation);
        sxml_host_value_release(document);
        if (validated == NULL) {
            sqlite3_result_error(context, "sxml: the document is not valid", -1);
            return;
        }
        document = validated;
    }
    sxml_item item = {0};
    sxml_node_reference node = {0};
    xdmb_bytes encoded = {0};
    sxml_status status = sxml_sequence_get_item(document->sequence, (sxml_item_request){0}, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(item, &node);
    }
    if (status == SXML_STATUS_OK) {
        xdmb_encode_request request = {
            .tree = node.tree,
            .type_registry = sxml_processor_get_type_registry(connection->processor->processor),
            .schema = connection->schema ? sxml_host_schema_compiled(connection->schema) : NULL,
            .keeps_locations = keeps_locations};
        status = xdmb_encode(&request, &encoded);
        connection->approximate_type_count = request.approximate_type_count;
    }
    if (status == SXML_STATUS_OK) {
        sqlite3_result_blob64(context, encoded.bytes, encoded.byte_count, SQLITE_TRANSIENT);
        xdmb_free(&encoded);
    } else {
        sqlite3_result_error(context, "sxml: cannot encode", -1);
    }
    sxml_host_value_release(document);
}

/* xml_exi(doc, alignment): the EXI stream of doc, with its options in the
   header so that every other function reads it without being told them.
   alignment: 0 bit-packed, 1 byte-alignment, 2 pre-compression, 3
   compression (sxml_exi_alignment). */
static void sqlite_xml_exi(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    if (sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
        return;
    }
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_value *document = sqlite_xml_document(context, connection->processor, arguments[0]);
    if (document == NULL) {
        return;
    }
    sxml_host_exi_encode_request request = {0};
    request.node = document;
    request.options.alignment = argument_count > 1 ? (uint32_t)sqlite3_value_int(arguments[1]) : 0;
    if (argument_count > 2 && sqlite3_value_int(arguments[2]) != 0) {
        /* full fidelity: what XDMB keeps (comments, PIs, prefixes) */
        request.options.preserves_comments = 1;
        request.options.preserves_processing_instructions = 1;
        request.options.preserves_prefixes = 1;
    }
    if (argument_count > 3 && sqlite3_value_int(arguments[3]) != 0) {
        /* schema-informed with the connection's schema; its options stay
           out of the header (xml_exi_schema names them for decoding) */
        request.options.schema = connection->exi_schema;
    } else {
        request.includes_options = 1;
    }
    if (sxml_host_exi_encode(connection->processor, &request) == SXML_STATUS_OK) {
        const sxml_byte_view *output = sxml_host_output_of(connection->processor);
        sqlite3_result_blob64(context, output->bytes, output->byte_count, SQLITE_TRANSIENT);
    } else {
        sqlite_xml_fail(context, connection->processor);
    }
    sxml_host_value_release(document);
}

/* xml_exi_schema(xsd, alignment): the schema (XSD text) and alignment of
   EXI streams written by xml_exi(doc, alignment, full, 1) and read without
   options in their header. Returns 1. */
static void sqlite_xml_exi_schema(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_schema *schema = sxml_host_schema_create(connection->processor);
    if (schema == NULL) {
        sqlite3_result_error_nomem(context);
        return;
    }
    const sxml_host_schema_document document = {
        .text = {sqlite3_value_text(arguments[0]), (size_t)sqlite3_value_bytes(arguments[0])}, .uri = {0}};
    sxml_host_schema_compile_request request = {.documents = &document, .document_count = 1, .base_uri = {0}};
    if (sxml_host_schema_compile(schema, &request) != SXML_STATUS_OK) {
        sqlite_xml_fail(context, connection->processor);
        sxml_host_schema_destroy(schema);
        return;
    }
    sxml_host_schema_destroy(connection->exi_schema);
    sxml_host_schema_destroy(connection->schema);
    connection->exi_schema = schema;
    connection->exi_alignment = (uint32_t)sqlite3_value_int(arguments[1]);
    sqlite3_result_int(context, 1);
}

/* xml_schema(xsd): compiles the schema xmlb(doc, 'validate') uses and
   typed blobs are opened with. Returns 1. */
static void sqlite_xml_schema(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_schema *schema = sxml_host_schema_create(connection->processor);
    if (schema == NULL) {
        sqlite3_result_error_nomem(context);
        return;
    }
    const sxml_host_schema_document document = {
        .text = {sqlite3_value_text(arguments[0]), (size_t)sqlite3_value_bytes(arguments[0])}, .uri = {0}};
    sxml_host_schema_compile_request request = {.documents = &document, .document_count = 1, .base_uri = {0}};
    if (sxml_host_schema_compile(schema, &request) != SXML_STATUS_OK) {
        sqlite_xml_fail(context, connection->processor);
        sxml_host_schema_destroy(schema);
        return;
    }
    sxml_host_schema_destroy(connection->schema);
    connection->schema = schema;
    sqlite3_result_int(context, 1);
}

/* xmlb_approximate(): how many type annotations the last xmlb wrote as a
   named base type (an anonymous type of a local declaration). */
static void sqlite_xml_approximate(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count, (void)arguments;
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sqlite3_result_int64(context, (sqlite3_int64)connection->approximate_type_count);
}

/* Writes value to the result: one atomic item as an SQL value, else
   serialized by the adaptive method. */
static void sqlite_xml_result(sqlite3_context *context, sxml_host_processor *processor,
                              const sxml_host_value *value, int serialize_nodes_as_xml) {
    uint64_t count = 0;
    if (sxml_sequence_count(value->sequence, NULL, &count) != SXML_STATUS_OK) {
        sqlite3_result_error(context, "sxml: cannot count the result", -1);
        return;
    }
    if (count == 0) {
        sqlite3_result_null(context);
        return;
    }
    sxml_item item = {0};
    if (count == 1 && sxml_sequence_get_item(value->sequence, (sxml_item_request){0}, &item) == SXML_STATUS_OK &&
        sxml_item_get_kind(item) == SXML_ITEM_KIND_ATOMIC) {
        sxml_atomic_datum datum = {0};
        if (sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK) {
            switch (datum.datum_kind) {
            case SXML_DATUM_KIND_BOOLEAN:
                sqlite3_result_int(context, datum.datum_details.boolean);
                return;
            case SXML_DATUM_KIND_INTEGER:
                sqlite3_result_int64(context, datum.datum_details.integer);
                return;
            case SXML_DATUM_KIND_DOUBLE:
            case SXML_DATUM_KIND_FLOAT:
                sqlite3_result_double(context, datum.datum_details.double_number);
                return;
            case SXML_DATUM_KIND_STRING:
                sqlite3_result_text64(context, (const char *)datum.datum_details.string.utf8.bytes,
                                      datum.datum_details.string.utf8.byte_count, SQLITE_TRANSIENT, SQLITE_UTF8);
                return;
            default:
                break;
            }
        }
    }
    (void)serialize_nodes_as_xml;
    if (sxml_host_serialize(processor, &(sxml_host_serialize_request){.value = value, .options = NULL}) !=
        SXML_STATUS_OK) {
        sqlite_xml_fail(context, processor);
        return;
    }
    const sxml_byte_view *output = sxml_host_output_of(processor);
    sqlite3_result_text64(context, (const char *)output->bytes, output->byte_count, SQLITE_TRANSIENT,
                          SQLITE_UTF8);
}

static void sqlite_xml_xml(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    if (sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
        return;
    }
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_value *document = sqlite_xml_document(context, connection->processor, arguments[0]);
    if (document == NULL) {
        return;
    }
    sqlite_xml_result(context, connection->processor, document, 1);
    sxml_host_value_release(document);
}

/* ---------------------------------------- xml_xpath, xml_exists, query */

typedef enum sqlite_xml_mode {
    SQLITE_XML_MODE_XPATH,
    SQLITE_XML_MODE_EXISTS,
    /* ->>: the expression's result atomized, data((EXPR)). */
    SQLITE_XML_MODE_ATOMIZE,
} sqlite_xml_mode;

static void sqlite_xml_destroy_expression(void *expression) {
    sxml_host_expression_destroy(expression);
}

static void sqlite_xml_destroy_query(void *query) {
    sxml_host_query_destroy(query);
}

static sxml_host_expression *sqlite_xml_expression(sqlite3_context *context, sxml_host_processor *processor,
                                                   sqlite_xml_mode mode, sqlite3_value *argument) {
    sxml_host_expression *expression = sqlite3_get_auxdata(context, 1);
    if (expression != NULL) {
        return expression;
    }
    const char *text = (const char *)sqlite3_value_text(argument);
    if (text == NULL) {
        return NULL;
    }
    char *wrapped = mode == SQLITE_XML_MODE_EXISTS    ? sqlite3_mprintf("boolean((%s\n))", text)
                    : mode == SQLITE_XML_MODE_ATOMIZE ? sqlite3_mprintf("data((%s\n))", text)
                                                      : sqlite3_mprintf("%s", text);
    expression = sxml_host_compile(
        processor, &(sxml_host_compile_request){
                       .expression_text = {(const uint8_t *)wrapped, strlen(wrapped)},
                   });
    sqlite3_free(wrapped);
    if (expression == NULL) {
        sqlite_xml_fail(context, processor);
        return NULL;
    }
    sqlite3_set_auxdata(context, 1, expression, sqlite_xml_destroy_expression);
    /* set_auxdata may have destroyed it already (out of memory). */
    return sqlite3_get_auxdata(context, 1);
}

static void sqlite_xml_evaluate(sqlite3_context *context, sqlite3_value **arguments, sqlite_xml_mode mode) {
    if (sqlite3_value_type(arguments[0]) == SQLITE_NULL || sqlite3_value_type(arguments[1]) == SQLITE_NULL) {
        return;
    }
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_processor *processor = connection->processor;
    sxml_host_expression *expression = sqlite_xml_expression(context, processor, mode, arguments[1]);
    if (expression == NULL) {
        return;
    }
    sxml_host_value *document = sqlite_xml_document(context, processor, arguments[0]);
    if (document == NULL) {
        return;
    }
    sxml_host_value *result =
        sxml_host_evaluate(expression, &(sxml_host_evaluate_request){.context_value = document});
    if (result == NULL) {
        sqlite_xml_fail(context, processor);
    } else {
        sqlite_xml_result(context, processor, result, 1);
        sxml_host_value_release(result);
    }
    sxml_host_value_release(document);
}

static void sqlite_xml_xpath(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    sqlite_xml_evaluate(context, arguments, SQLITE_XML_MODE_XPATH);
}

static void sqlite_xml_exists(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    sqlite_xml_evaluate(context, arguments, SQLITE_XML_MODE_EXISTS);
}

static void sqlite_xml_query(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    if (sqlite3_value_type(arguments[1]) == SQLITE_NULL) {
        return;
    }
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_processor *processor = connection->processor;
    sxml_host_query *query = sqlite3_get_auxdata(context, 1);
    if (query == NULL) {
        const char *text = (const char *)sqlite3_value_text(arguments[1]);
        query = sxml_host_query_compile(
            processor, &(sxml_host_query_compile_request){.query_text = {(const uint8_t *)text, strlen(text)}});
        if (query == NULL) {
            sqlite_xml_fail(context, processor);
            return;
        }
        sqlite3_set_auxdata(context, 1, query, sqlite_xml_destroy_query);
        query = sqlite3_get_auxdata(context, 1);
        if (query == NULL) {
            sqlite3_result_error_nomem(context);
            return;
        }
    }
    sxml_host_value *document = NULL;
    if (sqlite3_value_type(arguments[0]) != SQLITE_NULL) {
        document = sqlite_xml_document(context, processor, arguments[0]);
        if (document == NULL) {
            return;
        }
        if (sqlite3_user_data(context) == connection && argument_count > 2 && sqlite3_value_int(arguments[2]) != 0) {
            /* xml_query(doc, query, 1): over doc validated with the
               connection's schema, in memory (the reference the typed
               XDMB is compared with) */
            sxml_host_validate_request validation = {.node = document};
            sxml_host_value *validated =
                connection->schema ? sxml_host_schema_validate(connection->schema, &validation) : NULL;
            sxml_host_value_release(document);
            if (validated == NULL) {
                sqlite3_result_error(context, "sxml: not valid (or no xml_schema)", -1);
                return;
            }
            document = validated;
        }
    }
    sxml_host_query_evaluate_request request = {.context_value = document};
    sxml_host_value *result = sxml_host_query_evaluate(query, &request);
    if (result == NULL) {
        sqlite_xml_fail(context, processor);
    } else if (sxml_host_query_serialize(query, result) != SXML_STATUS_OK) {
        sqlite_xml_fail(context, processor);
    } else {
        const sxml_byte_view *output = sxml_host_output_of(processor);
        sqlite3_result_text64(context, (const char *)output->bytes, output->byte_count, SQLITE_TRANSIENT,
                              SQLITE_UTF8);
    }
    sxml_host_value_release(result);
    sxml_host_value_release(document);
}

/* ------------------------------------------------------------ Entry */

/* ---------------------------------------------- the -> and ->> operators */

/* The subtype SQLite's JSON functions give JSON text ('J', json.c), and
   ours for XML text. */
enum { SQLITE_XML_JSON_SUBTYPE = 74, SQLITE_XML_SUBTYPE = 88 };

/* Whether an operand is XML: text whose first character past white space
   (and a byte order mark) is '<', an XDMB blob, or a value of our XML
   subtype. JSON text never starts with '<'; JSONB is never "XDMB"... */
static int sqlite_xml_is_xml_operand(sqlite3_value *value) {
    if (sqlite3_value_subtype(value) == SQLITE_XML_SUBTYPE) {
        return 1;
    }
    if (sqlite3_value_type(value) == SQLITE_BLOB) {
        sxml_byte_view bytes = {sqlite3_value_blob(value), (size_t)sqlite3_value_bytes(value)};
        return xdmb_is_encoded(bytes);
    }
    if (sqlite3_value_type(value) != SQLITE_TEXT) {
        return 0;
    }
    const unsigned char *text = sqlite3_value_text(value);
    int length = sqlite3_value_bytes(value);
    int at = 0;
    if (length >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) {
        at = 3;
    }
    while (at < length && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
        at++;
    }
    return at < length && text[at] == '<';
}

/* SQLite's own operator on a JSON operand, answered by the private
   connection: its value exactly, and the JSON subtype where SQLite gives
   one (-> only). Subtypes do not cross connections, so that rule is ours;
   jsonarrow_test.py checks it. */
static void sqlite_xml_json_operator(sqlite3_context *context, sqlite3_value **arguments, int is_double) {
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    if (connection->json == NULL) {
        if (sqlite3_open_v2(":memory:", &connection->json, SQLITE_OPEN_READWRITE | SQLITE_OPEN_MEMORY, NULL) !=
                SQLITE_OK ||
            sqlite3_prepare_v2(connection->json, "select ?1 -> ?2", -1, &connection->json_arrow, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(connection->json, "select ?1 ->> ?2", -1, &connection->json_double_arrow,
                               NULL) != SQLITE_OK) {
            sqlite3_result_error(context, "sxml: cannot open the JSON connection", -1);
            return;
        }
    }
    sqlite3_stmt *statement = is_double ? connection->json_double_arrow : connection->json_arrow;
    sqlite3_bind_value(statement, 1, arguments[0]);
    sqlite3_bind_value(statement, 2, arguments[1]);
    int step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        sqlite3_result_value(context, sqlite3_column_value(statement, 0));
        /* ->: JSON text, of the JSON subtype; ->>: an SQL value, of none,
           objects and arrays included (jsonarrow_test.py) */
        if (!is_double && sqlite3_column_type(statement, 0) != SQLITE_NULL) {
            sqlite3_result_subtype(context, SQLITE_XML_JSON_SUBTYPE);
        }
    } else {
        sqlite3_result_error(context, sqlite3_errmsg(connection->json), -1);
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
}

/* doc -> expr: the XPath result as XML (nodes serialized, of our XML
   subtype; an atomic value as text); doc ->> expr: an SQL value, as
   xml_xpath gives, nodes atomized. Any other left operand: SQLite's. */
static void sqlite_xml_arrow(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    if (!sqlite_xml_is_xml_operand(arguments[0])) {
        sqlite_xml_json_operator(context, arguments, 0);
        return;
    }
    sqlite_xml_evaluate(context, arguments, SQLITE_XML_MODE_XPATH);
    if (sqlite3_value_type(arguments[1]) != SQLITE_NULL) {
        sqlite3_result_subtype(context, SQLITE_XML_SUBTYPE);
    }
}

static void sqlite_xml_double_arrow(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    if (!sqlite_xml_is_xml_operand(arguments[0])) {
        sqlite_xml_json_operator(context, arguments, 1);
        return;
    }
    sqlite_xml_evaluate(context, arguments, SQLITE_XML_MODE_ATOMIZE);
}

static void sqlite_xml_connection_destroy(void *state) {
    sqlite_xml_connection *connection = state;
    sqlite3_finalize(connection->json_arrow);
    sqlite3_finalize(connection->json_double_arrow);
    sqlite3_close(connection->json);
    sxml_host_schema_destroy(connection->exi_schema);
    sxml_host_processor_destroy(connection->processor);
    sqlite3_free(connection);
}

static int sqlite_xml_register(sqlite3 *database, char **error_message, int with_operators) {
    sqlite_xml_connection *connection = sqlite3_malloc(sizeof *connection);
    if (connection == NULL) {
        return SQLITE_NOMEM;
    }
    memset(connection, 0, sizeof *connection);
    connection->processor = sxml_host_processor_create(
        &(sxml_host_processor_configuration){.feature_mask = UINT32_MAX,
                                             .source_location_policy = SXML_SOURCE_LOCATION_POLICY_ALWAYS});
    if (connection->processor == NULL) {
        sqlite3_free(connection);
        *error_message = sqlite3_mprintf("sxml: cannot create a processor");
        return SQLITE_ERROR;
    }
    static const struct {
        const char *name;
        int argument_count;
        void (*function)(sqlite3_context *, int, sqlite3_value **);
        int flags;
    } all_functions[] = {
        /* the operators first, so that the last registration (owning the
           state) is the same either way */
        {"->", 2, sqlite_xml_arrow, SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE},
        {"->>", 2, sqlite_xml_double_arrow, SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE},
        {"xmlb", -1, sqlite_xml_xmlb, 0},        {"xml", 1, sqlite_xml_xml, 0},
        {"xml_schema", 1, sqlite_xml_schema, 0}, {"xmlb_approximate", 0, sqlite_xml_approximate, 0},
        {"xml_xpath", 2, sqlite_xml_xpath, 0},   {"xml_exists", 2, sqlite_xml_exists, 0},
        {"xml_query", -1, sqlite_xml_query, 0},   {"xml_exi", -1, sqlite_xml_exi, 0},
        {"xml_exi_schema", 2, sqlite_xml_exi_schema, 0},
    };
    const size_t first = with_operators ? 0 : 2;
    const size_t count = sizeof all_functions / sizeof all_functions[0];
    int result = SQLITE_OK;
    for (size_t index = first; index < count && result == SQLITE_OK; index++) {
        /* The last registration owns the connection state. */
        int is_last = index + 1 == count;
        result = sqlite3_create_function_v2(database, all_functions[index].name, all_functions[index].argument_count,
                                            SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS |
                                                all_functions[index].flags,
                                            connection, all_functions[index].function, NULL, NULL,
                                            is_last ? sqlite_xml_connection_destroy : NULL);
    }
    return result;
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_xml_init(sqlite3 *database, char **error_message, const sqlite3_api_routines *api) {
    SQLITE_EXTENSION_INIT2(api);
    return sqlite_xml_register(database, error_message, 0);
}

/* The named functions and the operators -> and ->>, opt-in because they
   replace SQLite's own on this connection (which still answer for every
   operand that is not XML): .load ./xml sqlite3_xml_operators_init */
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_xml_operators_init(sqlite3 *database, char **error_message, const sqlite3_api_routines *api) {
    SQLITE_EXTENSION_INIT2(api);
    return sqlite_xml_register(database, error_message, 1);
}
