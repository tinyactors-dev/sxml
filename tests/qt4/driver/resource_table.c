/*
 * The resource table of a QT4 test case (resource_table.h). Imitates the
 * suite resolver it replaces in test_run.c (packet 1.25) and the table
 * resolver of src/processor.c.
 */
#include "resource_table.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The URI scheme and authority of suite files: file:///SUITE-PATH. */
#define SXML_QT4_SUITE_URI_PREFIX "file:///"

/* A file to serve as bytes. */
typedef struct sxml_qt4_served_file {
    sxml_byte_view file_path;
    sxml_byte_view encoding;
} sxml_qt4_served_file;

/* Reads the file into *resource as BYTES, or marks it not available when
   it cannot be read. */
static sxml_status sxml_qt4_serve_file(const sxml_qt4_resource_table *table,
                                       sxml_qt4_served_file file,
                                       sxml_resource *resource) {
    sxml_byte_view contents = sxml_qt4_empty_text;
    const sxml_status status = sxml_qt4_catalog_read_file(
        table->catalog,
        &(sxml_qt4_file_request){.suite_path = file.file_path,
                                 .allocator = table->allocator},
        &contents);
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        resource->description = SXML_TEXT("The file cannot be read");
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = contents;
    resource->encoding_name = file.encoding;
    return SXML_STATUS_OK;
}

/* Serves file:///SUITE-PATH; any other URI is not available. */
static sxml_status
sxml_qt4_serve_suite_file(const sxml_qt4_resource_table *table,
                          sxml_byte_view uri, sxml_resource *resource) {
    const sxml_byte_view prefix = SXML_TEXT(SXML_QT4_SUITE_URI_PREFIX);
    if (uri.byte_count <= prefix.byte_count ||
        memcmp(uri.bytes, prefix.bytes, prefix.byte_count) != 0) {
        resource->description =
            SXML_TEXT("The QT4 driver serves only the suite's files and the "
                      "environment's resources");
        return SXML_STATUS_OK;
    }
    return sxml_qt4_serve_file(
        table,
        (sxml_qt4_served_file){
            .file_path = {.bytes = uri.bytes + prefix.byte_count,
                          .byte_count = uri.byte_count - prefix.byte_count},
            .encoding = sxml_qt4_empty_text},
        resource);
}

/* One request to the table, with the URI it is matched by. */
typedef struct sxml_qt4_table_request {
    const sxml_qt4_resource_table *table;
    const sxml_resource_request *request;
    sxml_byte_view uri;
} sxml_qt4_table_request;

/* TEXT and BINARY: the environment's resources first. */
static sxml_status
sxml_qt4_serve_resource(const sxml_qt4_table_request *table_request,
                        sxml_resource *resource) {
    const sxml_qt4_resource_table *table = table_request->table;
    const sxml_resource_request *request = table_request->request;
    const sxml_byte_view uri = table_request->uri;
    const sxml_qt4_served_resource_view resources = table->served_resources;
    for (size_t resource_index = 0;
         resource_index < resources.served_resource_count;
         resource_index += 1) {
        const sxml_qt4_served_resource *served =
            &resources.served_resources[resource_index];
        if (sxml_byte_view_equals(served->uri, uri)) {
            /* A binary resource is taken as it is (F&O 4.0,
               "fn:unparsed-binary"): no encoding. */
            return sxml_qt4_serve_file(
                table,
                (sxml_qt4_served_file){
                    .file_path = served->file_path,
                    .encoding = request->resource_kind ==
                                        SXML_RESOURCE_KIND_TEXT
                                    ? served->encoding
                                    : sxml_qt4_empty_text},
                resource);
        }
    }
    return sxml_qt4_serve_suite_file(table, uri, resource);
}

/* COLLECTION and URI_COLLECTION. */
static void
sxml_qt4_serve_collection(const sxml_qt4_table_request *table_request,
                          sxml_resource *resource) {
    const sxml_resource_request *request = table_request->request;
    const sxml_byte_view uri = table_request->uri;
    const sxml_qt4_served_collection_view collections =
        table_request->table->served_collections;
    for (size_t collection_index = 0;
         collection_index < collections.served_collection_count;
         collection_index += 1) {
        const sxml_qt4_served_collection *served =
            &collections.served_collections[collection_index];
        const bool is_match = request->is_default_collection
                                  ? served->is_default
                                  : !served->is_default &&
                                        sxml_byte_view_equals(served->uri, uri);
        if (!is_match) {
            continue;
        }
        const sxml_sequence items =
            request->resource_kind == SXML_RESOURCE_KIND_COLLECTION
                ? served->items
                : served->uris;
        /* sxml takes the reference (processor.h, sxml_resource). */
        sxml_sequence_retain(items);
        resource->resource_form = SXML_RESOURCE_FORM_SEQUENCE;
        resource->items = items;
        return;
    }
    resource->description =
        SXML_TEXT("The environment has no such collection");
}

/* The last segment of a URI reference: what follows its last "/". */
static sxml_byte_view sxml_qt4_last_segment(sxml_byte_view reference) {
    if (reference.byte_count == 0) {
        return reference;
    }
    size_t start = 0;
    for (size_t byte_index = 0; byte_index < reference.byte_count;
         byte_index += 1) {
        if (reference.bytes[byte_index] == '/') {
            start = byte_index + 1;
        }
    }
    return (sxml_byte_view){.bytes = reference.bytes + start,
                            .byte_count = reference.byte_count - start};
}

/* XQUERY_MODULE: the module of the target namespace that the location
   hint picks out best. */
static sxml_status
sxml_qt4_serve_module(const sxml_qt4_table_request *table_request,
                      sxml_resource *resource) {
    const sxml_resource_request *request = table_request->request;
    const sxml_qt4_served_module_view modules =
        table_request->table->served_modules;
    const sxml_byte_view hint = request->uri_reference;
    const sxml_byte_view hint_name = sxml_qt4_last_segment(hint);
    const sxml_qt4_served_module *chosen = NULL;
    /* 0: the first of the namespace, 1: by file name, 2: by location. */
    int chosen_rank = -1;
    for (size_t module_index = 0; module_index < modules.served_module_count;
         module_index += 1) {
        const sxml_qt4_served_module *module =
            &modules.served_modules[module_index];
        if (!sxml_byte_view_equals(module->namespace_uri,
                                   request->target_namespace_uri)) {
            continue;
        }
        int rank = 0;
        if (hint.byte_count != 0 && module->location.byte_count != 0 &&
            (sxml_byte_view_equals(module->location, hint) ||
             sxml_byte_view_equals(module->location, request->absolute_uri))) {
            rank = 2;
        } else if (hint_name.byte_count != 0 &&
                   sxml_byte_view_equals(
                       sxml_qt4_last_segment(module->file_path), hint_name)) {
            rank = 1;
        }
        if (rank > chosen_rank) {
            chosen = module;
            chosen_rank = rank;
        }
    }
    if (chosen == NULL) {
        resource->description =
            SXML_TEXT("The test case has no module of that namespace");
        return SXML_STATUS_OK;
    }
    const sxml_status status = sxml_qt4_serve_file(
        table_request->table,
        (sxml_qt4_served_file){.file_path = chosen->file_path,
                               .encoding = sxml_qt4_empty_text},
        resource);
    if (status == SXML_STATUS_OK &&
        resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        resource->resource_uri = chosen->file_uri;
    }
    return status;
}

/* SCHEMA_DOCUMENT: a suite file, else the environment's schema of the
   target namespace. */
static sxml_status
sxml_qt4_serve_schema(const sxml_qt4_table_request *table_request,
                      sxml_resource *resource) {
    const sxml_qt4_resource_table *table = table_request->table;
    sxml_status status =
        sxml_qt4_serve_suite_file(table, table_request->uri, resource);
    if (status != SXML_STATUS_OK ||
        resource->resource_form != SXML_RESOURCE_FORM_NONE) {
        return status;
    }
    const sxml_qt4_served_schema_view schemas = table->served_schemas;
    for (size_t schema_index = 0; schema_index < schemas.served_schema_count;
         schema_index += 1) {
        const sxml_qt4_served_schema *schema =
            &schemas.served_schemas[schema_index];
        if (!sxml_byte_view_equals(
                schema->namespace_uri,
                table_request->request->target_namespace_uri)) {
            continue;
        }
        status = sxml_qt4_serve_file(
            table,
            (sxml_qt4_served_file){.file_path = schema->file_path,
                                   .encoding = sxml_qt4_empty_text},
            resource);
        if (status == SXML_STATUS_OK &&
            resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
            resource->resource_uri = schema->file_uri;
        }
        return status;
    }
    resource->description =
        SXML_TEXT("The environment has no schema of that namespace");
    return SXML_STATUS_OK;
}

static sxml_status
sxml_qt4_resolve_from_table(void *state, const sxml_resource_request *request,
                            sxml_resource *resource) {
    const sxml_qt4_resource_table *table = state;
    const sxml_byte_view uri = request->absolute_uri.byte_count != 0
                                   ? request->absolute_uri
                                   : request->uri_reference;
    memset(resource, 0, sizeof *resource);
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    const sxml_qt4_table_request table_request = {
        .table = table, .request = request, .uri = uri};
    switch (request->resource_kind) {
    case SXML_RESOURCE_KIND_XML_DOCUMENT:
    case SXML_RESOURCE_KIND_EXTERNAL_ENTITY:
        /* External DTDs and entities are suite files too (D199). */
        return sxml_qt4_serve_suite_file(table, uri, resource);
    case SXML_RESOURCE_KIND_TEXT:
    case SXML_RESOURCE_KIND_BINARY:
        return sxml_qt4_serve_resource(&table_request, resource);
    case SXML_RESOURCE_KIND_COLLECTION:
    case SXML_RESOURCE_KIND_URI_COLLECTION:
        sxml_qt4_serve_collection(&table_request, resource);
        return SXML_STATUS_OK;
    case SXML_RESOURCE_KIND_XQUERY_MODULE:
        return sxml_qt4_serve_module(&table_request, resource);
    case SXML_RESOURCE_KIND_SCHEMA_DOCUMENT:
        return sxml_qt4_serve_schema(&table_request, resource);
    case SXML_RESOURCE_KIND_XSLT_MODULE:
        /* The modules a stylesheet of fn:transform includes and imports
           (packet 7.11, D203): the suite's files, as for documents. */
        return sxml_qt4_serve_suite_file(table, uri, resource);
    case SXML_RESOURCE_KIND_XSLT_PACKAGE:
        break;
    }
    resource->description =
        SXML_TEXT("The QT4 driver serves no XSLT packages");
    return SXML_STATUS_OK;
}

static void sxml_qt4_release_from_table(void *state,
                                        const sxml_resource *resource) {
    const sxml_qt4_resource_table *table = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(table->allocator,
                               (void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations
    sxml_qt4_resource_table_operations = {
        .resolve = sxml_qt4_resolve_from_table,
        .release = sxml_qt4_release_from_table,
};

sxml_resource_resolver
sxml_qt4_resource_table_resolver(const sxml_qt4_resource_table *table) {
    return (sxml_resource_resolver){
        .state = (void *)table, .operations = &sxml_qt4_resource_table_operations};
}
