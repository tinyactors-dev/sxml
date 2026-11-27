/*
 * The EXI golden test (tests/exi/README.md; D333): every case of
 * tests/exi/manifest.tsv through sxml's EXI decoder and encoder, against the
 * streams and decodings EXIficient made (the exi_oracle), with the
 * exi_ratchet of tests/exi/passing.txt.
 *
 *   exi_golden ROOT [--update] [--filter=TEXT] [--verbose] [--dump]
 *   exi_golden ROOT --self-check
 *
 * Each case is two checks, named <set>/<id>/decode and <set>/<id>/encode:
 *
 * - decode: the golden stream, decoded with the case's options known out of
 *   band, equals EXIficient's decoding of it (fn:deep-equal, comparing
 *   comments, processing instructions and prefixes when the options
 *   preserve them);
 * - encode: the case's input, encoded with the case's options, is the
 *   golden stream byte for byte; with the compression alignment, whose
 *   DEFLATE output may differ between implementations, the stream sxml
 *   writes is decoded again and compared as in decode. A case EXIficient
 *   refused (strict content its grammars cannot represent) must be refused.
 *
 * Exit status 1 when a check listed in passing.txt fails. --update adds the
 * checks that pass now to passing.txt (it removes none).
 *
 * --self-check tests the comparison itself, with no EXI code: each input of
 * the schema-less full-fidelity set must equal EXIficient's decoding of its
 * stream, and must differ from the decoding of another document.
 */
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Paths in the manifest are relative to the root: the driver enters it. */
#ifdef _WIN32
#include <direct.h>
#define SXML_EXI_GOLDEN_CHANGE_DIRECTORY _chdir
#else
#include <unistd.h>
#define SXML_EXI_GOLDEN_CHANGE_DIRECTORY chdir
#endif

#include "sxml/exi.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/serialize.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

enum {
    SXML_EXI_GOLDEN_FIELD_COUNT = 6,
    SXML_EXI_GOLDEN_MAXIMUM_SCHEMAS = 128,
    SXML_EXI_GOLDEN_MAXIMUM_NAMES = 16,
};

#define SXML_EXI_GOLDEN_TEXT(literal) \
    ((sxml_byte_view){.bytes = (const uint8_t *)(literal), .byte_count = sizeof(literal) - 1})

/* ------------------------------------------------------------- Memory */

/* The test's memory, from sxml's default allocator (no malloc outside
   src/allocator.c, docs/C_GUIDELINES.md). NULL when there is none. */
static void *sxml_exi_golden_allocate(size_t byte_count) {
    void *memory = NULL;
    if (sxml_allocator_allocate(sxml_default_allocator(),
                                (sxml_allocation_request){.byte_count = byte_count == 0 ? 1 : byte_count,
                                                          .alignment = alignof(max_align_t)},
                                &memory) != SXML_STATUS_OK) {
        return NULL;
    }
    return memory;
}

static void sxml_exi_golden_release(void *memory) {
    sxml_allocator_release(sxml_default_allocator(), memory);
}

/* A block to grow: its memory, the bytes in use and the bytes wanted. */
typedef struct sxml_exi_golden_growth {
    void *memory;
    size_t used_byte_count;
    size_t new_byte_count;
} sxml_exi_golden_growth;

/* Moves growth->memory's used bytes into a new block of new_byte_count;
   NULL (and the old block kept) when there is no memory. */
static void *sxml_exi_golden_grow(const sxml_exi_golden_growth *growth) {
    void *grown = sxml_exi_golden_allocate(growth->new_byte_count);
    if (grown == NULL) {
        return NULL;
    }
    if (growth->used_byte_count > 0) {
        memcpy(grown, growth->memory, growth->used_byte_count);
    }
    sxml_exi_golden_release(growth->memory);
    return grown;
}

/* ------------------------------------------------------------- Files */

typedef struct sxml_exi_golden_bytes {
    uint8_t *bytes;
    size_t byte_count;
} sxml_exi_golden_bytes;

/* Reads path (relative to root) whole; false when it cannot. */
static bool sxml_exi_golden_read_file(const char *path, sxml_exi_golden_bytes *file) {
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }
    size_t capacity = 65536;
    size_t byte_count = 0;
    uint8_t *bytes = sxml_exi_golden_allocate(capacity);
    while (bytes != NULL) {
        size_t read_count = fread(bytes + byte_count, 1, capacity - byte_count, stream);
        byte_count += read_count;
        if (byte_count < capacity) {
            break;
        }
        uint8_t *grown = sxml_exi_golden_grow(
            &(sxml_exi_golden_growth){.memory = bytes, .used_byte_count = byte_count, .new_byte_count = capacity * 2});
        if (grown == NULL) {
            sxml_exi_golden_release(bytes);
            bytes = NULL;
        } else {
            bytes = grown;
            capacity *= 2;
        }
    }
    (void)fclose(stream);
    if (bytes == NULL) {
        return false;
    }
    *file = (sxml_exi_golden_bytes){.bytes = bytes, .byte_count = byte_count};
    return true;
}

static sxml_byte_view sxml_exi_golden_view(const sxml_exi_golden_bytes *file) {
    return (sxml_byte_view){.bytes = file->bytes, .byte_count = file->byte_count};
}

static sxml_byte_view sxml_exi_golden_text(const char *text) {
    return (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = strlen(text)};
}

/* ------------------------------------------------------------ The run */

typedef struct sxml_exi_golden_schema {
    char *path;
    sxml_schema *schema;
} sxml_exi_golden_schema;

typedef struct sxml_exi_golden_run {
    const char *root;
    sxml_processor *processor;
    sxml_exi_decoder *decoder;
    sxml_exi_encoder *encoder;
    sxml_exi_golden_schema schemas[SXML_EXI_GOLDEN_MAXIMUM_SCHEMAS];
    size_t schema_count;
    bool is_verbose;
    /* --dump: write each decoding to standard error. */
    bool is_dumping;
    /* --encoded-dir=DIR: write each stream sxml encodes to DIR/<set>__<id>.exi,
       for the oracle to decode (tools/exi-oracle/README.md). */
    const char *encoded_directory;
} sxml_exi_golden_run;

/* The resolver of the run's processor: local files, by the path after
   "file://" or as given relative to the working directory (the root). */
static sxml_status sxml_exi_golden_resolve(void *state, const sxml_resource_request *request,
                                      sxml_resource *resource) {
    (void)state;
    sxml_byte_view uri = request->absolute_uri.byte_count > 0 ? request->absolute_uri : request->uri_reference;
    char path[4096];
    size_t offset = 0;
    if (uri.byte_count >= 7 && memcmp(uri.bytes, "file://", 7) == 0) {
        offset = 7;
    }
    if (uri.byte_count - offset >= sizeof path) {
        return SXML_STATUS_OK;
    }
    memcpy(path, uri.bytes + offset, uri.byte_count - offset);
    path[uri.byte_count - offset] = '\0';
    sxml_exi_golden_bytes file = {0};
    if (!sxml_exi_golden_read_file(path, &file)) {
        return SXML_STATUS_OK;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = sxml_exi_golden_view(&file);
    return SXML_STATUS_OK;
}

static void sxml_exi_golden_release_resource(void *state, const sxml_resource *resource) {
    (void)state;
    sxml_exi_golden_release((void *)resource->content.bytes);
}

static const sxml_resource_resolver_operations sxml_exi_golden_resolver_operations = {
    .resolve = sxml_exi_golden_resolve,
    .release = sxml_exi_golden_release_resource,
};

/* The compiled schema of path, compiled on first use; NULL when it does
   not compile. */
static sxml_schema *sxml_exi_golden_schema_of(sxml_exi_golden_run *run, const char *path) {
    for (size_t schema_index = 0; schema_index < run->schema_count; schema_index++) {
        if (strcmp(run->schemas[schema_index].path, path) == 0) {
            return run->schemas[schema_index].schema;
        }
    }
    if (run->schema_count == SXML_EXI_GOLDEN_MAXIMUM_SCHEMAS) {
        return NULL;
    }
    sxml_exi_golden_bytes file = {0};
    if (!sxml_exi_golden_read_file(path, &file)) {
        return NULL;
    }
    char uri[4200];
    (void)snprintf(uri, sizeof uri, "file://%s/%s", run->root, path);
    sxml_schema *schema = NULL;
    sxml_status status = sxml_schema_create(
        sxml_default_allocator(),
        &(sxml_schema_configuration){
            .processor = run->processor,
            .schema_documents = {.schema_documents =
                                     &(sxml_schema_document){
                                         .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                         .document_text = sxml_exi_golden_view(&file),
                                         .document_uri = sxml_exi_golden_text(uri)},
                                 .schema_document_count = 1}},
        &schema);
    sxml_exi_golden_release(file.bytes);
    if (status != SXML_STATUS_OK) {
        schema = NULL;
    }
    size_t path_length = strlen(path);
    char *path_copy = sxml_exi_golden_allocate(path_length + 1);
    if (path_copy == NULL) {
        sxml_schema_destroy(schema);
        return NULL;
    }
    memcpy(path_copy, path, path_length + 1);
    run->schemas[run->schema_count++] = (sxml_exi_golden_schema){.path = path_copy, .schema = schema};
    return schema;
}

/* The EXI schema resolver of the run: a schemaId is a path relative to the
   root, as the oracle wrote it. */
static sxml_status sxml_exi_golden_resolve_schema(void *state, const sxml_exi_schema_identifier *identifier,
                                             const sxml_schema **schema) {
    sxml_exi_golden_run *run = state;
    char path[4096];
    if (identifier->text.byte_count >= sizeof path) {
        *schema = NULL;
        return SXML_STATUS_OK;
    }
    memcpy(path, identifier->text.bytes, identifier->text.byte_count);
    path[identifier->text.byte_count] = '\0';
    *schema = sxml_exi_golden_schema_of(run, path);
    return SXML_STATUS_OK;
}

static const sxml_exi_schema_resolver_operations sxml_exi_golden_schema_resolver_operations = {
    .resolve = sxml_exi_golden_resolve_schema,
};

/* ------------------------------------------------------------ Options */

/* The options of a case, parsed from its specification (README.md). */
typedef struct sxml_exi_golden_case_options {
    sxml_exi_options options;
    sxml_exi_header_options header_options;
    sxml_exi_canonicalization canonicalization;
    char schema_path[4096];
    bool has_schema;
    sxml_name_identifier self_contained_names[SXML_EXI_GOLDEN_MAXIMUM_NAMES];
    size_t self_contained_name_count;
} sxml_exi_golden_case_options;

/* Splits text at separator in place, as strtok_r does (which C17 lacks):
   returns the next non-empty part and advances *cursor, NULL at the end. */
static char *sxml_exi_golden_next_part(char **cursor, char separator) {
    char *start = *cursor;
    while (start != NULL && *start == separator) {
        start++;
    }
    if (start == NULL || *start == '\0') {
        *cursor = NULL;
        return NULL;
    }
    char *end = strchr(start, separator);
    if (end != NULL) {
        *end = '\0';
        *cursor = end + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static bool sxml_exi_golden_has_item(const char *list, const char *item) {
    size_t item_length = strlen(item);
    const char *cursor = list;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, ',');
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
        if (length == item_length && memcmp(cursor, item, length) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

/* Parses a {uri}local list of names into the processor's pool. */
static bool sxml_exi_golden_parse_names(sxml_exi_golden_run *run, const char *value, sxml_exi_golden_case_options *case_options) {
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(run->processor);
    const char *cursor = value;
    while (*cursor != '\0' && case_options->self_contained_name_count < SXML_EXI_GOLDEN_MAXIMUM_NAMES) {
        const char *end = strchr(cursor, ',');
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
        sxml_byte_view uri = {0};
        sxml_byte_view local = {.bytes = (const uint8_t *)cursor, .byte_count = length};
        if (length > 0 && cursor[0] == '{') {
            const char *close = memchr(cursor, '}', length);
            if (close == NULL) {
                return false;
            }
            uri = (sxml_byte_view){.bytes = (const uint8_t *)cursor + 1, .byte_count = (size_t)(close - cursor - 1)};
            local = (sxml_byte_view){.bytes = (const uint8_t *)close + 1,
                                     .byte_count = length - (size_t)(close - cursor) - 1};
        }
        sxml_namespace_uri_identifier uri_identifier = {0};
        if (uri.byte_count > 0 &&
            sxml_name_pool_intern_namespace_uri(name_pool, uri, &uri_identifier) != SXML_STATUS_OK) {
            return false;
        }
        if (sxml_name_pool_intern_name(name_pool,
                                       &(sxml_name_request){.namespace_uri = uri_identifier, .local_name = local},
                                       &case_options->self_contained_names[case_options->self_contained_name_count]) !=
            SXML_STATUS_OK) {
            return false;
        }
        case_options->self_contained_name_count++;
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return true;
}

static bool sxml_exi_golden_parse_options(sxml_exi_golden_run *run, const char *specification,
                                     sxml_exi_golden_case_options *case_options) {
    *case_options = (sxml_exi_golden_case_options){0};
    char copy[8192];
    size_t specification_length = strlen(specification);
    if (specification_length >= sizeof copy) {
        return false;
    }
    memcpy(copy, specification, specification_length + 1);
    char *parts = copy;
    for (char *option = sxml_exi_golden_next_part(&parts, ';'); option != NULL; option = sxml_exi_golden_next_part(&parts, ';')) {
        char *part = option;
        char *value = strchr(part, '=');
        if (value != NULL) {
            *value++ = '\0';
        } else {
            value = part + strlen(part);
        }
        sxml_exi_options *options = &case_options->options;
        if (strcmp(part, "alignment") == 0) {
            if (strcmp(value, "byte-alignment") == 0) {
                options->alignment = SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT;
            } else if (strcmp(value, "pre-compression") == 0) {
                options->alignment = SXML_EXI_ALIGNMENT_PRE_COMPRESSION;
            } else if (strcmp(value, "compression") == 0) {
                options->alignment = SXML_EXI_ALIGNMENT_COMPRESSION;
            } else if (strcmp(value, "bit-packed") != 0) {
                return false;
            }
        } else if (strcmp(part, "preserve") == 0) {
            options->fidelity.preserves_comments = sxml_exi_golden_has_item(value, "comments");
            options->fidelity.preserves_processing_instructions = sxml_exi_golden_has_item(value, "pis");
            options->fidelity.preserves_dtd = sxml_exi_golden_has_item(value, "dtd");
            options->fidelity.preserves_prefixes = sxml_exi_golden_has_item(value, "prefixes");
            options->fidelity.preserves_lexical_values = sxml_exi_golden_has_item(value, "lexical-values");
        } else if (strcmp(part, "strict") == 0) {
            options->is_strict = true;
        } else if (strcmp(part, "fragment") == 0) {
            options->is_fragment = true;
        } else if (strcmp(part, "self-contained") == 0) {
            options->allows_self_contained = true;
            if (!sxml_exi_golden_parse_names(run, value, case_options)) {
                return false;
            }
        } else if (strcmp(part, "block-size") == 0) {
            options->has_block_size = true;
            options->block_size = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(part, "value-max-length") == 0) {
            options->has_value_max_length = true;
            options->value_max_length = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(part, "value-partition-capacity") == 0) {
            options->has_value_partition_capacity = true;
            options->value_partition_capacity = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(part, "schema") == 0) {
            size_t value_length = strlen(value);
            if (value_length >= sizeof case_options->schema_path) {
                return false;
            }
            memcpy(case_options->schema_path, value, value_length + 1);
            case_options->has_schema = true;
        } else if (strcmp(part, "xsd-types-only") == 0) {
            options->schema_identifier.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES;
        } else if (strcmp(part, "header") == 0) {
            case_options->header_options.includes_cookie = sxml_exi_golden_has_item(value, "cookie");
            case_options->header_options.includes_options = sxml_exi_golden_has_item(value, "options");
            case_options->header_options.includes_schema_identifier = sxml_exi_golden_has_item(value, "schema-id");
        } else if (strcmp(part, "canonical") == 0) {
            case_options->canonicalization.is_canonical = true;
        } else if (strcmp(part, "utc-time") == 0) {
            case_options->canonicalization.uses_utc_time = true;
        } else if (strcmp(part, "insignificant-xsi-nil") == 0) {
            /* EXIficient's encoder option; sxml writes every xsi:nil. */
        } else {
            return false;
        }
    }
    /* The oracle names the schema by its path; with schema-id in the
       header that path is the schemaId it writes. */
    if (case_options->has_schema && case_options->header_options.includes_schema_identifier) {
        case_options->options.schema_identifier = (sxml_exi_schema_identifier){
            .schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_TEXT,
            .text = sxml_exi_golden_text(case_options->schema_path)};
    }
    return true;
}

/* ---------------------------------------------------------- Comparison */

/* The document node of an XML document; false when it does not parse. */
static bool sxml_exi_golden_parse_xml(sxml_exi_golden_run *run, sxml_byte_view text, sxml_item *document) {
    return sxml_processor_parse_document(run->processor, &(sxml_xml_parse_request){.input = text}, document) ==
           SXML_STATUS_OK;
}

/* A stream to decode with a case's options. */
typedef struct sxml_exi_golden_decode_request {
    const sxml_exi_golden_case_options *case_options;
    sxml_byte_view stream;
} sxml_exi_golden_decode_request;

/* Decodes request->stream with request->case_options into a document. */
static sxml_status sxml_exi_golden_decode(sxml_exi_golden_run *run, const sxml_exi_golden_decode_request *request,
                                     sxml_item *document) {
    const sxml_exi_golden_case_options *case_options = request->case_options;
    const sxml_byte_view stream = request->stream;
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(),
        &(sxml_arena_tree_configuration){.allocator = sxml_default_allocator(),
                                         .item_heap = sxml_processor_get_item_heap(run->processor),
                                         .is_destroyed_when_unreferenced = true},
        &arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema *schema = case_options->has_schema ? sxml_exi_golden_schema_of(run, case_options->schema_path) : NULL;
    sxml_diagnostic diagnostic = {0};
    status = sxml_exi_decoder_decode(
        run->decoder,
        &(sxml_exi_decode_request){
            .input = stream,
            .options = &case_options->options,
            .schema = schema,
            .schema_resolver = {.state = run, .operations = &sxml_exi_golden_schema_resolver_operations},
            .diagnostic = &diagnostic},
        sxml_arena_tree_as_tree_builder(arena_tree));
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        if (run->is_verbose) {
            sxml_byte_view status_name = sxml_status_name(status);
            (void)fprintf(stderr, "    decode: %.*s: %.*s\n", (int)status_name.byte_count, (const char *)status_name.bytes,
                    (int)diagnostic.description.byte_count,
                    (const char *)diagnostic.description.bytes);
        }
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document = sxml_node_item_make((sxml_node_reference){.tree = tree});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* Two documents to compare, and the options whose fidelity says what. */
typedef struct sxml_exi_golden_comparison {
    const sxml_exi_options *options;
    sxml_item first;
    sxml_item second;
} sxml_exi_golden_comparison;

/* fn:deep-equal of two documents, with what the options preserve. */
static bool sxml_exi_golden_equal(sxml_exi_golden_run *run, const sxml_exi_golden_comparison *comparison) {
    const sxml_exi_options *options = comparison->options;
    const sxml_item first = comparison->first;
    const sxml_item second = comparison->second;
    char text[512];
    (void)snprintf(text, sizeof text,
             "deep-equal(doc('urn:first'), doc('urn:second'), {'comments': %s(), "
             "'processing-instructions': %s(), 'namespace-prefixes': %s()})",
             options->fidelity.preserves_comments ? "true" : "false",
             options->fidelity.preserves_processing_instructions ? "true" : "false",
             options->fidelity.preserves_prefixes ? "true" : "false");
    sxml_xpath_expression *expression = NULL;
    if (sxml_xpath_expression_create(sxml_default_allocator(),
                                     &(sxml_xpath_expression_configuration){.processor = run->processor,
                                                                            .expression_text = sxml_exi_golden_text(text)},
                                     &expression) != SXML_STATUS_OK) {
        return false;
    }
    const sxml_available_document documents[2] = {
        {.document_uri = SXML_EXI_GOLDEN_TEXT("urn:first"), .document_node = first},
        {.document_uri = SXML_EXI_GOLDEN_TEXT("urn:second"), .document_node = second},
    };
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = sxml_xpath_expression_evaluate(
        expression,
        &(sxml_xpath_evaluation_request){
            .dynamic_context = {.available_documents = {.available_documents = documents,
                                                        .available_document_count = 2}}},
        &result);
    sxml_xpath_expression_destroy(expression);
    bool is_equal = false;
    sxml_item item = {0};
    if (status == SXML_STATUS_OK &&
        sxml_sequence_get_item(result, (sxml_item_request){.item_index = 0}, &item) == SXML_STATUS_OK) {
        sxml_atomic_datum datum = {0};
        is_equal = sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
                   datum.datum_kind == SXML_DATUM_KIND_BOOLEAN && datum.datum_details.boolean;
    }
    sxml_sequence_release(result);
    return is_equal;
}

/* ---------------------------------------------------------------- Cases */

typedef struct sxml_exi_golden_case {
    char *fields[SXML_EXI_GOLDEN_FIELD_COUNT];
} sxml_exi_golden_case;

enum sxml_exi_golden_field {
    SXML_EXI_GOLDEN_FIELD_SET = 0,
    SXML_EXI_GOLDEN_FIELD_IDENTIFIER = 1,
    SXML_EXI_GOLDEN_FIELD_INPUT = 2,
    SXML_EXI_GOLDEN_FIELD_OPTIONS = 3,
    SXML_EXI_GOLDEN_FIELD_ORACLE = 4,
    SXML_EXI_GOLDEN_FIELD_DECODED = 5,
};

/* The outcome of one case's two checks. */
typedef struct sxml_exi_golden_outcome {
    bool has_decode_check;
    bool decode_passed;
    bool encode_passed;
} sxml_exi_golden_outcome;

/* --dump: the document, serialized, to standard error. */
static void sxml_exi_golden_dump(sxml_exi_golden_run *run, sxml_item document) {
    sxml_output_buffer *output_buffer = NULL;
    if (sxml_output_buffer_create(sxml_default_allocator(),
                                  &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator()},
                                  &output_buffer) != SXML_STATUS_OK) {
        return;
    }
    if (sxml_serialize_sequence(sxml_processor_get_item_heap(run->processor),
                                &(sxml_serialization_request){
                                    .value = sxml_sequence_from_item(document),
                                    .serialization_parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_XML}},
                                sxml_output_buffer_as_output_stream(output_buffer)) == SXML_STATUS_OK) {
        sxml_byte_view bytes = sxml_output_buffer_get_bytes(output_buffer);
        (void)fprintf(stderr, "%.*s\n", (int)bytes.byte_count, (const char *)bytes.bytes);
    }
    sxml_output_buffer_destroy(output_buffer);
}

static bool sxml_exi_golden_check_decode(sxml_exi_golden_run *run, const sxml_exi_golden_case *test_case,
                                    const sxml_exi_golden_case_options *case_options) {
    char path[4096];
    (void)snprintf(path, sizeof path, "tests/exi/golden/%s/%s.exi", test_case->fields[SXML_EXI_GOLDEN_FIELD_SET],
             test_case->fields[SXML_EXI_GOLDEN_FIELD_IDENTIFIER]);
    sxml_exi_golden_bytes stream = {0};
    sxml_exi_golden_bytes expected_text = {0};
    if (!sxml_exi_golden_read_file(path, &stream)) {
        return false;
    }
    bool passed = false;
    sxml_item decoded = {0};
    sxml_item expected = {0};
    if (sxml_exi_golden_read_file(test_case->fields[SXML_EXI_GOLDEN_FIELD_DECODED], &expected_text) &&
        sxml_exi_golden_parse_xml(run, sxml_exi_golden_view(&expected_text), &expected)) {
        if (sxml_exi_golden_decode(run, &(sxml_exi_golden_decode_request){.case_options = case_options, .stream = sxml_exi_golden_view(&stream)}, &decoded) == SXML_STATUS_OK) {
            passed = sxml_exi_golden_equal(run, &(sxml_exi_golden_comparison){.options = &case_options->options, .first = decoded, .second = expected});
            if (!passed && run->is_verbose) {
                (void)fprintf(stderr, "    decode: not deep-equal to the oracle's decoding\n");
            }
            if (run->is_dumping) {
                sxml_exi_golden_dump(run, decoded);
            }
            sxml_item_release(decoded);
        }
        sxml_item_release(expected);
    }
    sxml_exi_golden_release(stream.bytes);
    sxml_exi_golden_release(expected_text.bytes);
    return passed;
}

/* A case to encode: its manifest fields, its options, and whether the
   oracle refused it. */
typedef struct sxml_exi_golden_encode_check {
    const sxml_exi_golden_case *test_case;
    const sxml_exi_golden_case_options *case_options;
    bool is_refused;
} sxml_exi_golden_encode_check;

/* A stream to save, and its case. */
typedef struct sxml_exi_golden_saving {
    const sxml_exi_golden_case *test_case;
    sxml_byte_view bytes;
} sxml_exi_golden_saving;

static bool sxml_exi_golden_save(sxml_exi_golden_run *run, const sxml_exi_golden_saving *saving) {
    char path[4096];
    (void)snprintf(path, sizeof path, "%s/%s__%s.exi", run->encoded_directory,
                   saving->test_case->fields[SXML_EXI_GOLDEN_FIELD_SET],
                   saving->test_case->fields[SXML_EXI_GOLDEN_FIELD_IDENTIFIER]);
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) {
        return false;
    }
    bool is_written = fwrite(saving->bytes.bytes, 1, saving->bytes.byte_count, stream) == saving->bytes.byte_count;
    return fclose(stream) == 0 && is_written;
}

static bool sxml_exi_golden_check_encode(sxml_exi_golden_run *run, const sxml_exi_golden_encode_check *check) {
    const sxml_exi_golden_case *test_case = check->test_case;
    const sxml_exi_golden_case_options *case_options = check->case_options;
    const bool is_refused = check->is_refused;
    sxml_exi_golden_bytes input_text = {0};
    if (!sxml_exi_golden_read_file(test_case->fields[SXML_EXI_GOLDEN_FIELD_INPUT], &input_text)) {
        return false;
    }
    sxml_item input = {0};
    if (!sxml_exi_golden_parse_xml(run, sxml_exi_golden_view(&input_text), &input)) {
        sxml_exi_golden_release(input_text.bytes);
        return false;
    }
    sxml_node_reference root = {0};
    sxml_output_buffer *output_buffer = NULL;
    bool passed = false;
    sxml_status status = sxml_item_get_node(input, &root);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(sxml_default_allocator(),
                                           &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator()},
                                           &output_buffer);
    }
    const sxml_schema *schema = case_options->has_schema ? sxml_exi_golden_schema_of(run, case_options->schema_path) : NULL;
    sxml_diagnostic diagnostic = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_encode(
            run->encoder,
            &(sxml_exi_encode_request){
                .root = root,
                .options = &case_options->options,
                .schema = schema,
                .header_options = case_options->header_options,
                .canonicalization = case_options->canonicalization,
                .self_contained_elements = {.element_names = case_options->self_contained_names,
                                            .element_name_count = case_options->self_contained_name_count},
                .diagnostic = &diagnostic},
            sxml_output_buffer_as_output_stream(output_buffer));
        if (is_refused) {
            passed = status == SXML_STATUS_DYNAMIC_ERROR;
        } else if (status != SXML_STATUS_OK) {
            if (run->is_verbose) {
                sxml_byte_view status_name = sxml_status_name(status);
                (void)fprintf(stderr, "    encode: %.*s: %.*s\n", (int)status_name.byte_count,
                        (const char *)status_name.bytes,
                        (int)diagnostic.description.byte_count, (const char *)diagnostic.description.bytes);
            }
        } else if (run->encoded_directory != NULL &&
                   !sxml_exi_golden_save(run, &(sxml_exi_golden_saving){.test_case = test_case,
                                                                         .bytes = sxml_output_buffer_get_bytes(output_buffer)})) {
            passed = false;
        } else if (case_options->options.alignment == SXML_EXI_ALIGNMENT_COMPRESSION &&
                   !case_options->canonicalization.is_canonical) {
            /* DEFLATE output may differ: decode it and compare. */
            sxml_exi_golden_bytes expected_text = {0};
            sxml_item expected = {0};
            sxml_item decoded = {0};
            if (sxml_exi_golden_read_file(test_case->fields[SXML_EXI_GOLDEN_FIELD_DECODED], &expected_text) &&
                sxml_exi_golden_parse_xml(run, sxml_exi_golden_view(&expected_text), &expected)) {
                if (sxml_exi_golden_decode(run, &(sxml_exi_golden_decode_request){.case_options = case_options, .stream = sxml_output_buffer_get_bytes(output_buffer)}, &decoded) ==
                    SXML_STATUS_OK) {
                    passed = sxml_exi_golden_equal(run, &(sxml_exi_golden_comparison){.options = &case_options->options, .first = decoded, .second = expected});
                    sxml_item_release(decoded);
                }
                sxml_item_release(expected);
            }
            sxml_exi_golden_release(expected_text.bytes);
        } else {
            char path[4096];
            (void)snprintf(path, sizeof path, "tests/exi/golden/%s/%s.exi", test_case->fields[SXML_EXI_GOLDEN_FIELD_SET],
                     test_case->fields[SXML_EXI_GOLDEN_FIELD_IDENTIFIER]);
            sxml_exi_golden_bytes expected_stream = {0};
            if (sxml_exi_golden_read_file(path, &expected_stream)) {
                sxml_byte_view written = sxml_output_buffer_get_bytes(output_buffer);
                passed = written.byte_count == expected_stream.byte_count &&
                         memcmp(written.bytes, expected_stream.bytes, written.byte_count) == 0;
                if (!passed && run->is_verbose) {
                    size_t first_difference = 0;
                    while (first_difference < written.byte_count && first_difference < expected_stream.byte_count &&
                           written.bytes[first_difference] == expected_stream.bytes[first_difference]) {
                        first_difference++;
                    }
                    (void)fprintf(stderr, "    encode: %zu bytes, expected %zu, first difference at byte %zu\n",
                            written.byte_count, expected_stream.byte_count, first_difference);
                }
                sxml_exi_golden_release(expected_stream.bytes);
            }
        }
    }
    sxml_output_buffer_destroy(output_buffer);
    sxml_item_release(input);
    sxml_exi_golden_release(input_text.bytes);
    return passed;
}

/* ---------------------------------------------------------- The ratchet */

typedef struct sxml_exi_golden_key_list {
    char **keys;
    size_t key_count;
    size_t capacity;
} sxml_exi_golden_key_list;

static bool sxml_exi_golden_add_key(sxml_exi_golden_key_list *list, const char *key) {
    if (list->key_count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 1024 : list->capacity * 2;
        char **grown = (char **)sxml_exi_golden_grow(&(sxml_exi_golden_growth){
            .memory = (void *)list->keys,
            .used_byte_count = list->key_count * sizeof *list->keys,
            .new_byte_count = capacity * sizeof *list->keys});
        if (grown == NULL) {
            return false;
        }
        list->keys = grown;
        list->capacity = capacity;
    }
    size_t length = strlen(key);
    char *copy = sxml_exi_golden_allocate(length + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, key, length + 1);
    list->keys[list->key_count++] = copy;
    return true;
}

static int sxml_exi_golden_compare_keys(const void *first, const void *second) {
    return strcmp(*(char *const *)first, *(char *const *)second);
}

static bool sxml_exi_golden_has_key(const sxml_exi_golden_key_list *list, const char *key) {
    return list->key_count > 0 &&
           bsearch((const void *)&key, (const void *)list->keys, list->key_count, sizeof *list->keys, sxml_exi_golden_compare_keys) != NULL;
}

static void sxml_exi_golden_read_keys(const char *path, sxml_exi_golden_key_list *list) {
    FILE *stream = fopen(path, "r");
    if (stream == NULL) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof line, stream) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0' && line[0] != '#') {
            sxml_exi_golden_add_key(list, line);
        }
    }
    (void)fclose(stream);
    if (list->key_count > 0) {
        qsort((void *)list->keys, list->key_count, sizeof *list->keys, sxml_exi_golden_compare_keys);
    }
}

/* ---------------------------------------------------------------- Main */

typedef struct sxml_exi_golden_tally {
    char set[128];
    size_t decode_count;
    size_t decode_passed;
    size_t encode_count;
    size_t encode_passed;
} sxml_exi_golden_tally;

static void sxml_exi_golden_free_keys(sxml_exi_golden_key_list *list) {
    for (size_t key_index = 0; key_index < list->key_count; key_index++) {
        sxml_exi_golden_release(list->keys[key_index]);
    }
    sxml_exi_golden_release((void *)list->keys);
    *list = (sxml_exi_golden_key_list){0};
}

/* Releases everything the run made. */
static void sxml_exi_golden_finish(sxml_exi_golden_run *run) {
    for (size_t schema_index = 0; schema_index < run->schema_count; schema_index++) {
        sxml_schema_destroy(run->schemas[schema_index].schema);
        sxml_exi_golden_release(run->schemas[schema_index].path);
    }
    sxml_exi_encoder_destroy(run->encoder);
    sxml_exi_decoder_destroy(run->decoder);
    sxml_processor_destroy(run->processor);
}

/* --self-check: see the top of this file. Returns the exit status. */
static int sxml_exi_golden_self_check(sxml_exi_golden_run *run, char *manifest_text) {
    size_t checked_count = 0;
    size_t failed_count = 0;
    sxml_exi_golden_case_options full = {0};
    sxml_exi_golden_parse_options(run, "preserve=comments,pis,prefixes", &full);
    sxml_item previous = {0};
    bool has_previous = false;
    sxml_exi_golden_bytes previous_text = {0};
    char *lines = manifest_text;
    for (char *manifest_line = sxml_exi_golden_next_part(&lines, '\n'); manifest_line != NULL;
         manifest_line = sxml_exi_golden_next_part(&lines, '\n')) {
        char *line = manifest_line;
        char *fields[SXML_EXI_GOLDEN_FIELD_COUNT] = {0};
        char *cursor = line;
        for (size_t field_index = 0; field_index < SXML_EXI_GOLDEN_FIELD_COUNT && cursor != NULL; field_index++) {
            fields[field_index] = cursor;
            char *tab = strchr(cursor, '\t');
            cursor = tab == NULL ? NULL : tab + 1;
            if (tab != NULL) {
                *tab = '\0';
            }
        }
        if (line[0] == '#' || fields[SXML_EXI_GOLDEN_FIELD_DECODED] == NULL ||
            strcmp(fields[SXML_EXI_GOLDEN_FIELD_SET], "bit-packed-full") != 0 ||
            strcmp(fields[SXML_EXI_GOLDEN_FIELD_ORACLE], "ok") != 0) {
            continue;
        }
        sxml_exi_golden_bytes input_text = {0};
        sxml_exi_golden_bytes decoded_text = {0};
        sxml_item input = {0};
        sxml_item decoded = {0};
        if (!sxml_exi_golden_read_file(fields[SXML_EXI_GOLDEN_FIELD_INPUT], &input_text) ||
            !sxml_exi_golden_read_file(fields[SXML_EXI_GOLDEN_FIELD_DECODED], &decoded_text) ||
            !sxml_exi_golden_parse_xml(run, sxml_exi_golden_view(&input_text), &input) ||
            !sxml_exi_golden_parse_xml(run, sxml_exi_golden_view(&decoded_text), &decoded)) {
            (void)fprintf(stderr, "self-check: cannot read %s\n", fields[SXML_EXI_GOLDEN_FIELD_IDENTIFIER]);
            failed_count++;
            sxml_exi_golden_release(input_text.bytes);
            sxml_exi_golden_release(decoded_text.bytes);
            continue;
        }
        checked_count++;
        if (!sxml_exi_golden_equal(run, &(sxml_exi_golden_comparison){.options = &full.options, .first = input, .second = decoded})) {
            (void)fprintf(stderr, "self-check: %s differs from its decoding\n", fields[SXML_EXI_GOLDEN_FIELD_IDENTIFIER]);
            failed_count++;
        }
        bool is_same_text = previous_text.byte_count == decoded_text.byte_count &&
                            memcmp(previous_text.bytes, decoded_text.bytes, decoded_text.byte_count) == 0;
        if (has_previous && !is_same_text && sxml_exi_golden_equal(run, &(sxml_exi_golden_comparison){.options = &full.options, .first = previous, .second = decoded})) {
            (void)fprintf(stderr, "self-check: %s equals the previous document\n", fields[SXML_EXI_GOLDEN_FIELD_IDENTIFIER]);
            failed_count++;
        }
        if (has_previous) {
            sxml_item_release(previous);
        }
        previous = decoded;
        has_previous = true;
        sxml_item_release(input);
        sxml_exi_golden_release(input_text.bytes);
        sxml_exi_golden_release(previous_text.bytes);
        previous_text = decoded_text;
    }
    if (has_previous) {
        sxml_item_release(previous);
    }
    sxml_exi_golden_release(previous_text.bytes);
    (void)printf("self-check: %zu documents, %zu failures\n", checked_count, failed_count);
    return failed_count == 0 && checked_count > 0 ? 0 : 1;
}

int main(int argument_count, char **arguments) {
    if (argument_count < 2) {
        (void)fprintf(stderr, "usage: exi_golden ROOT [--update] [--filter=TEXT] [--verbose]\n");
        return 2;
    }
    sxml_exi_golden_run run = {.root = arguments[1]};
    bool is_update = false;
    bool is_self_check = false;
    const char *filter = NULL;
    for (int argument_index = 2; argument_index < argument_count; argument_index++) {
        if (strcmp(arguments[argument_index], "--update") == 0) {
            is_update = true;
        } else if (strncmp(arguments[argument_index], "--filter=", 9) == 0) {
            filter = arguments[argument_index] + 9;
        } else if (strncmp(arguments[argument_index], "--encoded-dir=", 14) == 0) {
            run.encoded_directory = arguments[argument_index] + 14;
        } else if (strcmp(arguments[argument_index], "--dump") == 0) {
            run.is_dumping = true;
        } else if (strcmp(arguments[argument_index], "--self-check") == 0) {
            is_self_check = true;
        } else if (strcmp(arguments[argument_index], "--verbose") == 0) {
            run.is_verbose = true;
        } else {
            (void)fprintf(stderr, "exi_golden: unknown argument '%s'\n", arguments[argument_index]);
            return 2;
        }
    }
    /* Paths in the manifest are relative to the root. */
    if (SXML_EXI_GOLDEN_CHANGE_DIRECTORY(run.root) != 0) {
        (void)fprintf(stderr, "exi_golden: cannot enter '%s'\n", run.root);
        return 2;
    }
    sxml_allocator allocator = sxml_default_allocator();
    if (sxml_processor_create(allocator,
                              &(sxml_processor_configuration){
                                  .allocator = allocator,
                                  .resource_resolver = {.operations = &sxml_exi_golden_resolver_operations}},
                              &run.processor) != SXML_STATUS_OK ||
        sxml_exi_decoder_create(allocator,
                                &(sxml_exi_decoder_configuration){
                                    .allocator = allocator,
                                    .name_pool = sxml_processor_get_name_pool(run.processor)},
                                &run.decoder) != SXML_STATUS_OK ||
        sxml_exi_encoder_create(allocator, &(sxml_exi_encoder_configuration){.allocator = allocator},
                                &run.encoder) != SXML_STATUS_OK) {
        (void)fprintf(stderr, "exi_golden: cannot create the processor, decoder and encoder\n");
        return 2;
    }

    sxml_exi_golden_bytes manifest = {0};
    if (!sxml_exi_golden_read_file("tests/exi/manifest.tsv", &manifest)) {
        (void)fprintf(stderr, "exi_golden: cannot read tests/exi/manifest.tsv\n");
        return 2;
    }
    sxml_exi_golden_key_list passing = {0};
    sxml_exi_golden_read_keys("tests/exi/passing.txt", &passing);
    sxml_exi_golden_key_list passed_now = {0};
    sxml_exi_golden_tally tallies[64] = {0};
    size_t tally_count = 0;
    size_t regression_count = 0;

    char *text = sxml_exi_golden_allocate(manifest.byte_count + 1);
    if (text == NULL) {
        sxml_exi_golden_free_keys(&passing);
        sxml_exi_golden_release(manifest.bytes);
        sxml_exi_golden_finish(&run);
        return 2;
    }
    memcpy(text, manifest.bytes, manifest.byte_count);
    text[manifest.byte_count] = '\0';
    sxml_exi_golden_release(manifest.bytes);
    if (is_self_check) {
        int self_check_status = sxml_exi_golden_self_check(&run, text);
        sxml_exi_golden_release(text);
        sxml_exi_golden_free_keys(&passing);
        sxml_exi_golden_finish(&run);
        return self_check_status;
    }
    char *lines = text;
    for (char *manifest_line = sxml_exi_golden_next_part(&lines, '\n'); manifest_line != NULL;
         manifest_line = sxml_exi_golden_next_part(&lines, '\n')) {
        char *line = manifest_line;
        if (line[0] == '#') {
            continue;
        }
        sxml_exi_golden_case test_case = {0};
        char *cursor = line;
        for (size_t field_index = 0; field_index < SXML_EXI_GOLDEN_FIELD_COUNT; field_index++) {
            test_case.fields[field_index] = cursor;
            char *tab = cursor == NULL ? NULL : strchr(cursor, '\t');
            if (tab != NULL) {
                *tab = '\0';
                cursor = tab + 1;
            } else {
                cursor = NULL;
            }
        }
        if (test_case.fields[SXML_EXI_GOLDEN_FIELD_ORACLE] == NULL) {
            continue;
        }
        if (test_case.fields[SXML_EXI_GOLDEN_FIELD_DECODED] == NULL) {
            test_case.fields[SXML_EXI_GOLDEN_FIELD_DECODED] = "";
        }
        char key[1024];
        (void)snprintf(key, sizeof key, "%s/%s", test_case.fields[SXML_EXI_GOLDEN_FIELD_SET],
                 test_case.fields[SXML_EXI_GOLDEN_FIELD_IDENTIFIER]);
        if (filter != NULL && strstr(key, filter) == NULL) {
            continue;
        }
        bool is_refused = strncmp(test_case.fields[SXML_EXI_GOLDEN_FIELD_ORACLE], "refused", 7) == 0;
        bool is_ok = strcmp(test_case.fields[SXML_EXI_GOLDEN_FIELD_ORACLE], "ok") == 0;
        if (!is_ok && !is_refused) {
            continue;
        }
        sxml_exi_golden_case_options case_options = {0};
        sxml_exi_golden_outcome outcome = {0};
        if (!sxml_exi_golden_parse_options(&run, test_case.fields[SXML_EXI_GOLDEN_FIELD_OPTIONS], &case_options)) {
            (void)fprintf(stderr, "%s: cannot read the options '%s'\n", key, test_case.fields[SXML_EXI_GOLDEN_FIELD_OPTIONS]);
        } else {
            if (run.is_verbose) {
                (void)fprintf(stderr, "%s\n", key);
            }
            outcome.has_decode_check = is_ok;
            if (is_ok) {
                outcome.decode_passed = sxml_exi_golden_check_decode(&run, &test_case, &case_options);
            }
            outcome.encode_passed = sxml_exi_golden_check_encode(
                &run, &(sxml_exi_golden_encode_check){.test_case = &test_case, .case_options = &case_options, .is_refused = is_refused});
        }
        size_t tally_index = 0;
        while (tally_index < tally_count && strcmp(tallies[tally_index].set, test_case.fields[SXML_EXI_GOLDEN_FIELD_SET]) != 0) {
            tally_index++;
        }
        if (tally_index == tally_count && tally_count < 64) {
            (void)snprintf(tallies[tally_count].set, sizeof tallies[tally_count].set, "%s",
                     test_case.fields[SXML_EXI_GOLDEN_FIELD_SET]);
            tally_count++;
        }
        sxml_exi_golden_tally *tally = tally_index < 64 ? &tallies[tally_index] : NULL;
        const char *checks[2] = {"decode", "encode"};
        const bool has_check[2] = {outcome.has_decode_check, true};
        const bool passed[2] = {outcome.decode_passed, outcome.encode_passed};
        for (size_t check_index = 0; check_index < 2; check_index++) {
            if (!has_check[check_index]) {
                continue;
            }
            char check_key[1100];
            (void)snprintf(check_key, sizeof check_key, "%s/%s", key, checks[check_index]);
            if (tally != NULL) {
                if (check_index == 0) {
                    tally->decode_count++;
                    tally->decode_passed += passed[0] ? 1 : 0;
                } else {
                    tally->encode_count++;
                    tally->encode_passed += passed[1] ? 1 : 0;
                }
            }
            if (passed[check_index]) {
                sxml_exi_golden_add_key(&passed_now, check_key);
            } else if (sxml_exi_golden_has_key(&passing, check_key)) {
                (void)fprintf(stderr, "REGRESSION %s\n", check_key);
                regression_count++;
            }
        }
    }

    size_t decode_total = 0;
    size_t decode_passed_total = 0;
    size_t encode_total = 0;
    size_t encode_passed_total = 0;
    (void)printf("%-28s %15s %15s\n", "set", "decode", "encode");
    for (size_t tally_index = 0; tally_index < tally_count; tally_index++) {
        const sxml_exi_golden_tally *tally = &tallies[tally_index];
        (void)printf("%-28s %7zu / %-5zu %7zu / %-5zu\n", tally->set, tally->decode_passed, tally->decode_count,
               tally->encode_passed, tally->encode_count);
        decode_total += tally->decode_count;
        decode_passed_total += tally->decode_passed;
        encode_total += tally->encode_count;
        encode_passed_total += tally->encode_passed;
    }
    (void)printf("%-28s %7zu / %-5zu %7zu / %-5zu\n", "all", decode_passed_total, decode_total, encode_passed_total,
           encode_total);
    if (passed_now.key_count > 0) {
        qsort((void *)passed_now.keys, passed_now.key_count, sizeof *passed_now.keys, sxml_exi_golden_compare_keys);
    }
    size_t new_count = 0;
    for (size_t key_index = 0; key_index < passed_now.key_count; key_index++) {
        if (!sxml_exi_golden_has_key(&passing, passed_now.keys[key_index])) {
            new_count++;
        }
    }
    (void)printf("ratchet: %zu listed, %zu regressions, %zu newly passing%s\n", passing.key_count, regression_count,
           new_count, new_count > 0 && !is_update ? " (run with --update to list them)" : "");
    if (is_update && regression_count == 0 && filter == NULL) {
        sxml_exi_golden_key_list merged = {0};
        for (size_t key_index = 0; key_index < passing.key_count; key_index++) {
            sxml_exi_golden_add_key(&merged, passing.keys[key_index]);
        }
        for (size_t key_index = 0; key_index < passed_now.key_count; key_index++) {
            if (!sxml_exi_golden_has_key(&passing, passed_now.keys[key_index])) {
                sxml_exi_golden_add_key(&merged, passed_now.keys[key_index]);
            }
        }
        if (merged.key_count > 0) {
            qsort((void *)merged.keys, merged.key_count, sizeof *merged.keys, sxml_exi_golden_compare_keys);
        }
        FILE *stream = fopen("tests/exi/passing.txt", "w");
        if (stream == NULL) {
            return 2;
        }
        (void)fprintf(stream, "# The exi_ratchet (tests/exi/README.md): checks that must keep passing.\n");
        (void)fprintf(stream, "# Written by exi_golden --update; never remove a line by hand.\n");
        for (size_t key_index = 0; key_index < merged.key_count; key_index++) {
            (void)fprintf(stream, "%s\n", merged.keys[key_index]);
        }
        (void)fclose(stream);
        (void)printf("ratchet: passing.txt now lists %zu checks\n", merged.key_count);
        sxml_exi_golden_free_keys(&merged);
    } else if (is_update && filter != NULL) {
        (void)printf("ratchet: --update ignores a run with --filter\n");
    }
    sxml_exi_golden_release(text);
    sxml_exi_golden_free_keys(&passing);
    sxml_exi_golden_free_keys(&passed_now);
    sxml_exi_golden_finish(&run);
    return regression_count > 0 ? 1 : 0;
}
