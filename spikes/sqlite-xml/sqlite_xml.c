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
                                 .allocator = processor->allocator},
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

static void sqlite_xml_xmlb(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    (void)argument_count;
    if (sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
        return;
    }
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_value *document = sqlite_xml_document(context, connection->processor, arguments[0]);
    if (document == NULL) {
        return;
    }
    sxml_item item = {0};
    sxml_node_reference node = {0};
    xdmb_bytes encoded = {0};
    sxml_status status = sxml_sequence_get_item(document->sequence, (sxml_item_request){0}, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(item, &node);
    }
    if (status == SXML_STATUS_OK) {
        status = xdmb_encode(node.tree, &encoded);
    }
    if (status == SXML_STATUS_OK) {
        sqlite3_result_blob64(context, encoded.bytes, encoded.byte_count, SQLITE_TRANSIENT);
        xdmb_free(&encoded);
    } else {
        sqlite3_result_error(context, "sxml: cannot encode", -1);
    }
    sxml_host_value_release(document);
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
    char *wrapped = mode == SQLITE_XML_MODE_EXISTS ? sqlite3_mprintf("boolean((%s\n))", text)
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

static void sqlite_xml_connection_destroy(void *state) {
    sqlite_xml_connection *connection = state;
    sxml_host_processor_destroy(connection->processor);
    sqlite3_free(connection);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_xml_init(sqlite3 *database, char **error_message, const sqlite3_api_routines *api) {
    SQLITE_EXTENSION_INIT2(api);
    sqlite_xml_connection *connection = sqlite3_malloc(sizeof *connection);
    if (connection == NULL) {
        return SQLITE_NOMEM;
    }
    connection->processor = sxml_host_processor_create(
        &(sxml_host_processor_configuration){.feature_mask = UINT32_MAX});
    if (connection->processor == NULL) {
        sqlite3_free(connection);
        *error_message = sqlite3_mprintf("sxml: cannot create a processor");
        return SQLITE_ERROR;
    }
    static const struct {
        const char *name;
        int argument_count;
        void (*function)(sqlite3_context *, int, sqlite3_value **);
    } functions[] = {
        {"xmlb", 1, sqlite_xml_xmlb},         {"xml", 1, sqlite_xml_xml},
        {"xml_xpath", 2, sqlite_xml_xpath},   {"xml_exists", 2, sqlite_xml_exists},
        {"xml_query", 2, sqlite_xml_query},
    };
    int result = SQLITE_OK;
    for (size_t index = 0; index < sizeof functions / sizeof functions[0] && result == SQLITE_OK; index++) {
        /* The last registration owns the connection state. */
        int is_last = index + 1 == sizeof functions / sizeof functions[0];
        result = sqlite3_create_function_v2(database, functions[index].name, functions[index].argument_count,
                                            SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, connection,
                                            functions[index].function, NULL, NULL,
                                            is_last ? sqlite_xml_connection_destroy : NULL);
    }
    return result;
}
