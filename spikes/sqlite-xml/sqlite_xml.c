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
    /* What the last xmlparse wrote as an approximation. */
    size_t approximate_type_count;
    /* The xmlfiles table a running xmlquery/xmlexists reads (its third
       argument), NULL for none; and the database it is in. */
    char *files_table;
    sqlite3 *database;
    /* Turns XDMB and EXI rows into XML text for the resolver, which may
       not call the processor it resolves for (D71). */
    sxml_host_processor *helper;
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
    sxml_host_processor_destroy(connection->helper);
    sqlite3_free(connection->files_table);
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



/* ------------------------------------------------- xmlfiles: the files */

/*
 * CREATE VIRTUAL TABLE docs USING xmlfiles: the files a query may read,
 * kept as FTS5 keeps its own: in a shadow table, docs_content(uri,
 * content, media_type), which the module owns (xShadowName), so that a
 * database in defensive mode refuses SQL that writes it directly. The
 * virtual table is the way in: SELECT, INSERT, UPDATE, DELETE on docs.
 * content is stored as given: XML text, XDMB, EXI, or any text or bytes.
 * xmlquery(query, doc, 'docs') resolves fn:doc, fn:unparsed-text, module
 * and schema imports against docs' rows, by uri.
 */

/* Whether the xmlfiles table named by argument exists; sets it as the
   table the coming evaluation reads. */
static int sqlite_xml_use_files(sqlite3_context *context, sqlite_xml_connection *connection,
                                sqlite3_value *argument) {
    const char *name = (const char *)sqlite3_value_text(argument);
    char *sql = name ? sqlite3_mprintf("select 1 from \"%w_content\" limit 0", name) : NULL;
    sqlite3_stmt *statement = NULL;
    int result = sql ? sqlite3_prepare_v2(connection->database, sql, -1, &statement, NULL) : SQLITE_MISUSE;
    sqlite3_finalize(statement);
    sqlite3_free(sql);
    if (result != SQLITE_OK) {
        char *message = sqlite3_mprintf("sxml: no xmlfiles table named %s", name ? name : "NULL");
        sqlite3_result_error(context, message, -1);
        sqlite3_free(message);
        return SQLITE_ERROR;
    }
    connection->files_table = sqlite3_mprintf("%s", name);
    return connection->files_table ? SQLITE_OK : SQLITE_NOMEM;
}

/* A row's content as XML text when it is XDMB or EXI, through the helper
   processor; NULL with nothing to free when it is neither. The result is
   the helper's output, valid until its next operation. */
static const sxml_byte_view *sqlite_xml_row_as_text(sqlite_xml_connection *connection, sxml_byte_view bytes) {
    int is_xdmb = xdmb_is_encoded(bytes), is_exi = !is_xdmb && sqlite_xml_is_exi(bytes);
    if (!is_xdmb && !is_exi) {
        return NULL;
    }
    if (connection->helper == NULL) {
        connection->helper = sxml_host_processor_create(&(sxml_host_processor_configuration){.feature_mask = UINT32_MAX});
        if (connection->helper == NULL) {
            return NULL;
        }
    }
    sxml_host_processor *helper = connection->helper;
    sxml_host_value *value = NULL;
    if (is_xdmb) {
        sxml_tree *tree = NULL;
        if (xdmb_open(&(xdmb_open_request){.bytes = bytes,
                                           .item_heap = sxml_processor_get_item_heap(helper->processor),
                                           .allocator = helper->allocator,
                                           .type_registry = sxml_processor_get_type_registry(helper->processor)},
                      &tree) != SXML_STATUS_OK) {
            return NULL;
        }
        sxml_item item = sxml_node_item_make((sxml_node_reference){.tree = tree, .node_index = {0}});
        sxml_tree_release(tree);
        value = sxml_host_value_make(helper, sxml_sequence_from_item(item));
    } else {
        value = sxml_host_exi_parse(helper, &(sxml_host_exi_parse_request){.input = bytes});
    }
    if (value == NULL) {
        return NULL;
    }
    sxml_host_serialize_request request = {.value = value, .options = NULL};
    uint32_t status = sxml_host_serialize(helper, &request);
    sxml_host_value_release(value);
    return status == SXML_STATUS_OK ? sxml_host_output_of(helper) : NULL;
}

/* The resolver of the connection's processor (sxml_host_resolver): a row
   of the xmlfiles table the running query names, by its uri (the
   reference as written, else the absolute URI). Reads the database while
   the calling statement runs, which SQLite allows; it writes nothing. */
static uint32_t sqlite_xml_resolve(void *state, const sxml_host_resource_request *request,
                                   sxml_host_resource *resource) {
    sqlite_xml_connection *connection = state;
    if (connection->files_table == NULL) {
        return SXML_STATUS_OK;
    }
    char *sql = sqlite3_mprintf("select content from \"%w_content\" where uri = ?1", connection->files_table);
    sqlite3_stmt *statement = NULL;
    if (sql == NULL || sqlite3_prepare_v2(connection->database, sql, -1, &statement, NULL) != SQLITE_OK) {
        sqlite3_free(sql);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    sqlite3_free(sql);
    static const char base[] = "xmlfiles:/";
    sxml_byte_view below = {0};
    if (request->absolute_uri.byte_count > sizeof base - 1 &&
        memcmp(request->absolute_uri.bytes, base, sizeof base - 1) == 0) {
        below = (sxml_byte_view){request->absolute_uri.bytes + sizeof base - 1,
                                 request->absolute_uri.byte_count - (sizeof base - 1)};
    }
    const sxml_byte_view uris[2] = {below, request->uri_reference};
    uint32_t status = SXML_STATUS_OK;
    for (int attempt = 0; attempt < 2 && resource->resource_form == 0; attempt++) {
        if (uris[attempt].byte_count == 0) {
            continue;
        }
        sqlite3_bind_text64(statement, 1, (const char *)uris[attempt].bytes, uris[attempt].byte_count,
                            SQLITE_STATIC, SQLITE_UTF8);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            sxml_byte_view bytes = {sqlite3_column_blob(statement, 0), (size_t)sqlite3_column_bytes(statement, 0)};
            const sxml_byte_view *text = sqlite_xml_row_as_text(connection, bytes);
            if (text != NULL) {
                bytes = *text;
            }
            void *copy = sxml_host_allocate(request->processor, bytes.byte_count ? bytes.byte_count : 1);
            if (copy == NULL) {
                status = SXML_STATUS_OUT_OF_MEMORY;
                break;
            }
            memcpy(copy, bytes.bytes, bytes.byte_count);
            resource->resource_form = 1;
            resource->content = (sxml_byte_view){copy, bytes.byte_count};
        }
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    return status;
}

/* The virtual table: its schema is the shadow table's, minus nothing. */
typedef struct sqlite_xml_files {
    sqlite3_vtab base;
    sqlite3 *database;
    char *schema_name;
    char *table_name;
} sqlite_xml_files;

typedef struct sqlite_xml_files_cursor {
    sqlite3_vtab_cursor base;
    sqlite3_stmt *statement;
    int at_end;
} sqlite_xml_files_cursor;

static int sqlite_xml_files_connect_or_create(sqlite3 *database, void *auxiliary, int argument_count,
                                              const char *const *arguments, sqlite3_vtab **table, char **error,
                                              int creates) {
    (void)auxiliary;
    if (argument_count != 3) {
        *error = sqlite3_mprintf("xmlfiles takes no arguments");
        return SQLITE_ERROR;
    }
    if (creates) {
        char *sql = sqlite3_mprintf(
            "create table \"%w\".\"%w_content\"(uri text primary key, content blob not null, media_type text)",
            arguments[1], arguments[2]);
        int result = sql ? sqlite3_exec(database, sql, NULL, NULL, error) : SQLITE_NOMEM;
        sqlite3_free(sql);
        if (result != SQLITE_OK) {
            return result;
        }
    }
    int result = sqlite3_declare_vtab(database, "create table x(uri text, content blob, media_type text)");
    if (result != SQLITE_OK) {
        return result;
    }
    sqlite_xml_files *files = sqlite3_malloc(sizeof *files);
    if (files == NULL) {
        return SQLITE_NOMEM;
    }
    memset(files, 0, sizeof *files);
    files->database = database;
    files->schema_name = sqlite3_mprintf("%s", arguments[1]);
    files->table_name = sqlite3_mprintf("%s", arguments[2]);
    *table = &files->base;
    return SQLITE_OK;
}

static int sqlite_xml_files_create(sqlite3 *database, void *auxiliary, int argument_count,
                                   const char *const *arguments, sqlite3_vtab **table, char **error) {
    return sqlite_xml_files_connect_or_create(database, auxiliary, argument_count, arguments, table, error, 1);
}

static int sqlite_xml_files_connect(sqlite3 *database, void *auxiliary, int argument_count,
                                    const char *const *arguments, sqlite3_vtab **table, char **error) {
    return sqlite_xml_files_connect_or_create(database, auxiliary, argument_count, arguments, table, error, 0);
}

static int sqlite_xml_files_disconnect(sqlite3_vtab *table) {
    sqlite_xml_files *files = (sqlite_xml_files *)table;
    sqlite3_free(files->schema_name);
    sqlite3_free(files->table_name);
    sqlite3_free(files);
    return SQLITE_OK;
}

static int sqlite_xml_files_destroy(sqlite3_vtab *table) {
    sqlite_xml_files *files = (sqlite_xml_files *)table;
    char *sql = sqlite3_mprintf("drop table \"%w\".\"%w_content\"", files->schema_name, files->table_name);
    int result = sql ? sqlite3_exec(files->database, sql, NULL, NULL, NULL) : SQLITE_NOMEM;
    sqlite3_free(sql);
    if (result == SQLITE_OK) {
        sqlite_xml_files_disconnect(table);
    }
    return result;
}

/* uri = ? is a lookup by the shadow table's primary key; anything else
   a scan. */
static int sqlite_xml_files_best_index(sqlite3_vtab *table, sqlite3_index_info *info) {
    (void)table;
    info->idxNum = 0;
    info->estimatedCost = 1000000;
    for (int index = 0; index < info->nConstraint; index++) {
        const struct sqlite3_index_constraint *constraint = &info->aConstraint[index];
        if (constraint->usable && constraint->iColumn == 0 && constraint->op == SQLITE_INDEX_CONSTRAINT_EQ) {
            info->idxNum = 1;
            info->aConstraintUsage[index].argvIndex = 1;
            info->aConstraintUsage[index].omit = 1;
            info->estimatedCost = 1;
            info->estimatedRows = 1;
            info->idxFlags = SQLITE_INDEX_SCAN_UNIQUE;
            break;
        }
    }
    return SQLITE_OK;
}

static int sqlite_xml_files_open(sqlite3_vtab *table, sqlite3_vtab_cursor **cursor) {
    (void)table;
    sqlite_xml_files_cursor *files_cursor = sqlite3_malloc(sizeof *files_cursor);
    if (files_cursor == NULL) {
        return SQLITE_NOMEM;
    }
    memset(files_cursor, 0, sizeof *files_cursor);
    *cursor = &files_cursor->base;
    return SQLITE_OK;
}

static int sqlite_xml_files_close(sqlite3_vtab_cursor *cursor) {
    sqlite_xml_files_cursor *files_cursor = (sqlite_xml_files_cursor *)cursor;
    sqlite3_finalize(files_cursor->statement);
    sqlite3_free(files_cursor);
    return SQLITE_OK;
}

static int sqlite_xml_files_next(sqlite3_vtab_cursor *cursor) {
    sqlite_xml_files_cursor *files_cursor = (sqlite_xml_files_cursor *)cursor;
    int result = sqlite3_step(files_cursor->statement);
    files_cursor->at_end = result != SQLITE_ROW;
    return result == SQLITE_ROW || result == SQLITE_DONE ? SQLITE_OK : result;
}

static int sqlite_xml_files_filter(sqlite3_vtab_cursor *cursor, int index_number, const char *index_text,
                                   int argument_count, sqlite3_value **arguments) {
    (void)index_text, (void)argument_count;
    sqlite_xml_files_cursor *files_cursor = (sqlite_xml_files_cursor *)cursor;
    sqlite_xml_files *files = (sqlite_xml_files *)cursor->pVtab;
    sqlite3_finalize(files_cursor->statement);
    files_cursor->statement = NULL;
    char *sql = sqlite3_mprintf("select rowid, uri, content, media_type from \"%w\".\"%w_content\"%s",
                                files->schema_name, files->table_name, index_number == 1 ? " where uri = ?1" : "");
    int result = sql ? sqlite3_prepare_v2(files->database, sql, -1, &files_cursor->statement, NULL) : SQLITE_NOMEM;
    sqlite3_free(sql);
    if (result != SQLITE_OK) {
        return result;
    }
    if (index_number == 1) {
        sqlite3_bind_value(files_cursor->statement, 1, arguments[0]);
    }
    return sqlite_xml_files_next(cursor);
}

static int sqlite_xml_files_eof(sqlite3_vtab_cursor *cursor) {
    return ((sqlite_xml_files_cursor *)cursor)->at_end;
}

static int sqlite_xml_files_column(sqlite3_vtab_cursor *cursor, sqlite3_context *context, int column) {
    sqlite3_result_value(context, sqlite3_column_value(((sqlite_xml_files_cursor *)cursor)->statement, column + 1));
    return SQLITE_OK;
}

static int sqlite_xml_files_rowid(sqlite3_vtab_cursor *cursor, sqlite3_int64 *rowid) {
    *rowid = sqlite3_column_int64(((sqlite_xml_files_cursor *)cursor)->statement, 0);
    return SQLITE_OK;
}

/* INSERT, UPDATE and DELETE on the virtual table, applied to the shadow
   table. */
static int sqlite_xml_files_update(sqlite3_vtab *table, int argument_count, sqlite3_value **arguments,
                                   sqlite3_int64 *rowid) {
    sqlite_xml_files *files = (sqlite_xml_files *)table;
    char *sql = NULL;
    if (argument_count == 1) {
        sql = sqlite3_mprintf("delete from \"%w\".\"%w_content\" where rowid = ?1", files->schema_name,
                              files->table_name);
    } else if (sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
        sql = sqlite3_mprintf("insert into \"%w\".\"%w_content\"(rowid, uri, content, media_type) values (?2, ?3, ?4, ?5)",
                              files->schema_name, files->table_name);
    } else {
        sql = sqlite3_mprintf("update \"%w\".\"%w_content\" set rowid = ?2, uri = ?3, content = ?4, media_type = ?5 "
                              "where rowid = ?1",
                              files->schema_name, files->table_name);
    }
    sqlite3_stmt *statement = NULL;
    int result = sql ? sqlite3_prepare_v2(files->database, sql, -1, &statement, NULL) : SQLITE_NOMEM;
    sqlite3_free(sql);
    if (result != SQLITE_OK) {
        return result;
    }
    for (int index = 0; index < argument_count && index < 5; index++) {
        if (!(index == 0 && argument_count > 1 && sqlite3_value_type(arguments[0]) == SQLITE_NULL)) {
            sqlite3_bind_value(statement, index + 1, arguments[index]);
        }
    }
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        table->zErrMsg = sqlite3_mprintf("%s", sqlite3_errmsg(files->database));
        return result == SQLITE_CONSTRAINT ? SQLITE_CONSTRAINT : SQLITE_ERROR;
    }
    if (argument_count > 1 && sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
        *rowid = sqlite3_last_insert_rowid(files->database);
    }
    return SQLITE_OK;
}

static int sqlite_xml_files_rename(sqlite3_vtab *table, const char *new_name) {
    sqlite_xml_files *files = (sqlite_xml_files *)table;
    char *sql = sqlite3_mprintf("alter table \"%w\".\"%w_content\" rename to \"%w_content\"", files->schema_name,
                                files->table_name, new_name);
    int result = sql ? sqlite3_exec(files->database, sql, NULL, NULL, NULL) : SQLITE_NOMEM;
    sqlite3_free(sql);
    if (result == SQLITE_OK) {
        sqlite3_free(files->table_name);
        files->table_name = sqlite3_mprintf("%s", new_name);
    }
    return result;
}

/* The shadow table is the module's own: SQLite refuses to let ordinary
   SQL write it while the database is in defensive mode. */
static int sqlite_xml_files_shadow_name(const char *suffix) {
    return strcmp(suffix, "content") == 0;
}

static const sqlite3_module sqlite_xml_files_module = {
    .iVersion = 3,
    .xCreate = sqlite_xml_files_create,
    .xConnect = sqlite_xml_files_connect,
    .xBestIndex = sqlite_xml_files_best_index,
    .xDisconnect = sqlite_xml_files_disconnect,
    .xDestroy = sqlite_xml_files_destroy,
    .xOpen = sqlite_xml_files_open,
    .xClose = sqlite_xml_files_close,
    .xFilter = sqlite_xml_files_filter,
    .xNext = sqlite_xml_files_next,
    .xEof = sqlite_xml_files_eof,
    .xColumn = sqlite_xml_files_column,
    .xRowid = sqlite_xml_files_rowid,
    .xUpdate = sqlite_xml_files_update,
    .xRename = sqlite_xml_files_rename,
    .xShadowName = sqlite_xml_files_shadow_name,
};

/* What xmlquery, xmlexists and xmlquery_validated do with the result. */
typedef enum sqlite_xml_run_mode {
    /* xmlquery(query, doc [, files]): one atomic item as an SQL value,
       anything else serialized (SQL/XML XMLQUERY). */
    SQLITE_XML_RUN_QUERY,
    /* xmlexists(query, doc [, files]): 1 when the result is not empty,
       else 0 (SQL/XML XMLEXISTS: not the effective boolean value). */
    SQLITE_XML_RUN_EXISTS,
    /* xmlquery_validated(query, doc): over doc validated in memory with
       the connection's schema (xmlschema), the reference typed XDMB is
       compared with (fidelity_test.py). Spike only. */
    SQLITE_XML_RUN_VALIDATED,
} sqlite_xml_run_mode;

/* The query (argument 0, compiled once per statement as XQuery) over doc
   (argument 1). A third argument names an xmlfiles table whose rows
   fn:doc, fn:unparsed-text, module imports and schema imports read: the
   functions with it are registered as not deterministic, those without
   it read nothing and are deterministic. */
static void sqlite_xml_run(sqlite3_context *context, int argument_count, sqlite3_value **arguments,
                           sqlite_xml_run_mode mode) {
    if (sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
        return;
    }
    sqlite_xml_connection *connection = sqlite3_user_data(context);
    sxml_host_processor *processor = connection->processor;
    int reads_files = mode != SQLITE_XML_RUN_VALIDATED && argument_count > 2;
    if (reads_files && sqlite_xml_use_files(context, connection, arguments[2]) != SQLITE_OK) {
        return;
    }
    sxml_host_query *query = sqlite3_get_auxdata(context, 0);
    if (query == NULL) {
        const char *text = (const char *)sqlite3_value_text(arguments[0]);
        /* With files, relative references resolve against xmlfiles:/,
           so that a row's uri is the path below it (lib/tax.xqm imports
           rates.xqm as lib/rates.xqm). Module and schema imports are
           resolved here, at compile time. */
        static const char base[] = "xmlfiles:/";
        query = sxml_host_query_compile(
            processor, &(sxml_host_query_compile_request){
                           .query_text = {(const uint8_t *)text, strlen(text)},
                           .static_base_uri = reads_files ? (sxml_byte_view){(const uint8_t *)base, sizeof base - 1}
                                                          : (sxml_byte_view){0}});
        if (query == NULL) {
            sqlite3_free(connection->files_table);
            connection->files_table = NULL;
            sqlite_xml_fail(context, processor);
            return;
        }
        sqlite3_set_auxdata(context, 0, query, sqlite_xml_destroy_query);
        query = sqlite3_get_auxdata(context, 0);
        if (query == NULL) {
            sqlite3_free(connection->files_table);
            connection->files_table = NULL;
            sqlite3_result_error_nomem(context);
            return;
        }
    }
    sxml_host_value *document = NULL;
    if (sqlite3_value_type(arguments[1]) != SQLITE_NULL) {
        document = sqlite_xml_document(context, processor, arguments[1]);
        if (document == NULL) {
            sqlite3_free(connection->files_table);
            connection->files_table = NULL;
            return;
        }
        if (mode == SQLITE_XML_RUN_VALIDATED) {
            sxml_host_validate_request validation = {.node = document};
            sxml_host_value *validated =
                connection->schema ? sxml_host_schema_validate(connection->schema, &validation) : NULL;
            sxml_host_value_release(document);
            if (validated == NULL) {
                sqlite3_result_error(context, "sxml: not valid (or no xmlschema)", -1);
                return;
            }
            document = validated;
        }
    }
    sxml_host_query_evaluate_request request = {.context_value = document};
    sxml_host_value *result = sxml_host_query_evaluate(query, &request);
    sqlite3_free(connection->files_table);
    connection->files_table = NULL;
    if (result == NULL) {
        sqlite_xml_fail(context, processor);
    } else if (mode == SQLITE_XML_RUN_EXISTS) {
        uint64_t count = 0;
        if (sxml_sequence_count(result->sequence, NULL, &count) != SXML_STATUS_OK) {
            sqlite3_result_error(context, "sxml: cannot count the result", -1);
        } else {
            sqlite3_result_int(context, count > 0);
        }
    } else {
        sqlite_xml_result(context, processor, result, 1);
    }
    sxml_host_value_release(result);
    sxml_host_value_release(document);
}

static void sqlite_xml_query(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    sqlite_xml_run(context, argument_count, arguments, SQLITE_XML_RUN_QUERY);
}

static void sqlite_xml_exists_query(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    sqlite_xml_run(context, argument_count, arguments, SQLITE_XML_RUN_EXISTS);
}

static void sqlite_xml_query_validated(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
    sqlite_xml_run(context, argument_count, arguments, SQLITE_XML_RUN_VALIDATED);
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
    connection->database = database;
    connection->processor = sxml_host_processor_create(
        &(sxml_host_processor_configuration){.feature_mask = UINT32_MAX,
                                             .resolver = {.state = connection, .resolve = sqlite_xml_resolve},
                                             .source_location_policy = SXML_SOURCE_LOCATION_POLICY_ALWAYS});
    if (connection->processor == NULL) {
        sqlite3_free(connection);
        *error_message = sqlite3_mprintf("sxml: cannot create a processor");
        return SQLITE_ERROR;
    }
    /* Names and argument order of SQL/XML (ISO/IEC 9075-14) where it has
       the function; the rest in its style. Deterministic and innocuous
       unless marked: those reading an xmlfiles table (a third argument)
       are neither, so they can be in no index, and are direct-only. */
    enum { READS_FILES = 1 };
    static const struct {
        const char *name;
        int argument_count;
        void (*function)(sqlite3_context *, int, sqlite3_value **);
        int flags;
        int reads;
    } all_functions[] = {
        /* the operators first, so that the last registration (owning the
           state) is the same either way */
        {"->", 2, sqlite_xml_arrow, SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE, 0},
        {"->>", 2, sqlite_xml_double_arrow, SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE, 0},
        {"xmlparse", 1, sqlite_xml_xmlb, 0, 0},
        {"xmlparse", 2, sqlite_xml_xmlb, 0, 0},
        {"xmlserialize", 1, sqlite_xml_xml, 0, 0},
        {"xmlquery", 2, sqlite_xml_query, SQLITE_RESULT_SUBTYPE, 0},
        {"xmlquery", 3, sqlite_xml_query, SQLITE_RESULT_SUBTYPE, READS_FILES},
        {"xmlexists", 2, sqlite_xml_exists_query, 0, 0},
        {"xmlexists", 3, sqlite_xml_exists_query, 0, READS_FILES},
        {"xmlexi", -1, sqlite_xml_exi, 0, 0},
        /* spike only */
        {"xmlschema", 1, sqlite_xml_schema, 0, 0},
        {"xmlparse_approximate", 0, sqlite_xml_approximate, 0, 0},
        {"xmlquery_validated", 2, sqlite_xml_query_validated, 0, 0},
        {"xmlexi_schema", 2, sqlite_xml_exi_schema, 0, 0},
    };
    const size_t first = with_operators ? 0 : 2;
    const size_t count = sizeof all_functions / sizeof all_functions[0];
    int result = SQLITE_OK;
    for (size_t index = first; index < count && result == SQLITE_OK; index++) {
        /* The last registration owns the connection state. */
        int is_last = index + 1 == count;
        int flags = all_functions[index].reads ? SQLITE_DIRECTONLY
                                               : SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS;
        result = sqlite3_create_function_v2(database, all_functions[index].name, all_functions[index].argument_count,
                                            SQLITE_UTF8 | flags | all_functions[index].flags, connection,
                                            all_functions[index].function, NULL, NULL,
                                            is_last ? sqlite_xml_connection_destroy : NULL);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_create_module(database, "xmlfiles", &sqlite_xml_files_module, NULL);
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
