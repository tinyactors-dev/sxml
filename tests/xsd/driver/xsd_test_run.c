/*
 * Running one XSD 1.1 test (xsd_test_run.h). Imitates
 * tests/xslt/driver/xslt_test_run.c and xslt_execution.c: the decision
 * first, then an isolated run in a processor of its own through the
 * shared test budget (budget.h).
 */
#include "xsd_test_run.h"

#include "budget.h"
#include "reason.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xsd_test_empty_text = {.bytes = NULL,
                                                        .byte_count = 0};

/* As the QT4 driver: entity expansion bounded, nesting not. */
enum { SXML_XSD_TEST_RUN_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* What running a test decided. */
typedef struct sxml_xsd_test_answer {
    sxml_qt4_test_outcome outcome;
    sxml_qt4_reason *reason;
} sxml_xsd_test_answer;

/* Ends the reason with "STAGE[ SUBJECT]: STATUS" and makes the outcome an
   error. */
typedef struct sxml_xsd_test_failure {
    sxml_byte_view stage;
    sxml_byte_view subject;
    sxml_status status;
} sxml_xsd_test_failure;

static void sxml_xsd_test_fail(sxml_xsd_test_answer *answer,
                               sxml_xsd_test_failure failure) {
    answer->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
    sxml_qt4_reason_append(answer->reason, failure.stage);
    if (failure.subject.byte_count != 0) {
        sxml_qt4_reason_append(answer->reason, SXML_TEXT(" "));
        sxml_qt4_reason_append(answer->reason, failure.subject);
    }
    sxml_qt4_reason_append(answer->reason, SXML_TEXT(": "));
    sxml_qt4_reason_append(answer->reason,
                           failure.status == SXML_STATUS_INPUT_OUTPUT_FAILED
                               ? SXML_TEXT("cannot read")
                               : sxml_status_name(failure.status));
}

/* ------------------------------------------------------------ Documents */

/* The URI of a suite document: file:///SUITE-PATH (xsd_test_run.h). */
static const sxml_byte_view sxml_xsd_test_uri_prefix =
    SXML_BYTE_VIEW_INITIALIZER("file:///");

/* The XLink schema document xsts.xsd imports, and its vendored copy
   relative to the suite's directory (third_party/xsdtests). */
static const sxml_byte_view sxml_xsd_test_xlink_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/XML/2008/06/xlink.xsd");
static const sxml_byte_view sxml_xsd_test_xlink_path =
    SXML_BYTE_VIEW_INITIALIZER("../w3c-xlink/xlink.xsd");

/* Serves the suite's files to the schema assembly: an absolute URI
   file:///SUITE-PATH is the suite file SUITE-PATH, and the W3C's XLink
   schema document, which the suite's own xsts.xsd imports (the
   introspection set), is the copy vendored beside the suite
   (third_party/w3c-xlink, D278), as the library serves its stand-in
   xml.xsd; anything else is not available. */
typedef struct sxml_xsd_test_resolver_state {
    sxml_xsd_test_catalog *catalog;
    sxml_allocator allocator;
} sxml_xsd_test_resolver_state;

static sxml_status sxml_xsd_test_resolve(void *state,
                                         const sxml_resource_request *request,
                                         sxml_resource *resource) {
    const sxml_xsd_test_resolver_state *resolver = state;
    memset(resource, 0, sizeof(*resource));
    const sxml_byte_view uri = request->absolute_uri;
    const size_t prefix_count = sxml_xsd_test_uri_prefix.byte_count;
    const bool is_xlink = sxml_byte_view_equals(uri, sxml_xsd_test_xlink_uri);
    if (request->resource_kind != SXML_RESOURCE_KIND_SCHEMA_DOCUMENT ||
        (!is_xlink &&
         (uri.byte_count <= prefix_count ||
          memcmp(uri.bytes, sxml_xsd_test_uri_prefix.bytes, prefix_count) != 0))) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        return SXML_STATUS_OK;
    }
    sxml_byte_view contents = sxml_xsd_test_empty_text;
    const sxml_status status = sxml_xsd_test_catalog_read_file(
        resolver->catalog,
        &(sxml_qt4_file_request){
            .suite_path = is_xlink ? sxml_xsd_test_xlink_path
                                   : (sxml_byte_view){.bytes = uri.bytes + prefix_count,
                                                      .byte_count = uri.byte_count - prefix_count},
            .allocator = resolver->allocator},
        &contents);
    if (status == SXML_STATUS_OUT_OF_MEMORY) {
        return status;
    }
    if (status != SXML_STATUS_OK) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        return SXML_STATUS_OK;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = contents;
    return SXML_STATUS_OK;
}

static void sxml_xsd_test_release_resource(void *state,
                                           const sxml_resource *resource) {
    const sxml_xsd_test_resolver_state *resolver = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(resolver->allocator,
                               (void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations sxml_xsd_test_resolver_operations =
    {.resolve = sxml_xsd_test_resolve, .release = sxml_xsd_test_release_resource};

/* The documents of a test read into memory, with their URIs. */
typedef struct sxml_xsd_test_documents {
    sxml_schema_document *schema_documents;
    size_t document_count;
    /* All uri_bytes of the documents, one allocation. */
    uint8_t *uri_bytes;
} sxml_xsd_test_documents;

typedef struct sxml_xsd_test_document_request {
    sxml_xsd_test_catalog *catalog;
    sxml_allocator allocator;
    sxml_xsd_test_document_view documents;
} sxml_xsd_test_document_request;

static void sxml_xsd_test_release_documents(sxml_allocator allocator,
                                            sxml_xsd_test_documents *documents) {
    for (size_t document_index = 0; document_index < documents->document_count;
         document_index += 1) {
        sxml_allocator_release(
            allocator,
            (void *)documents->schema_documents[document_index].document_text.bytes);
    }
    sxml_allocator_release(allocator, documents->schema_documents);
    sxml_allocator_release(allocator, documents->uri_bytes);
    *documents = (sxml_xsd_test_documents){
        .schema_documents = NULL, .document_count = 0, .uri_bytes = NULL};
}

/* Reads every document of a view as TEXT with document URI
   file:///SUITE-PATH; a document that cannot be read fails the answer. */
static bool sxml_xsd_test_read_documents(const sxml_xsd_test_document_request *request,
                                         sxml_xsd_test_documents *documents,
                                         sxml_xsd_test_answer *answer) {
    *documents = (sxml_xsd_test_documents){
        .schema_documents = NULL, .document_count = 0, .uri_bytes = NULL};
    const size_t count = request->documents.file_path_count;
    if (count == 0) {
        return true;
    }
    size_t uri_byte_count = 0;
    for (size_t document_index = 0; document_index < count; document_index += 1) {
        uri_byte_count += sxml_xsd_test_uri_prefix.byte_count +
                          request->documents.file_paths[document_index].byte_count;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request->allocator,
        (sxml_allocation_request){.byte_count = count * sizeof(sxml_schema_document),
                                  .alignment = _Alignof(sxml_schema_document)},
        &memory);
    documents->schema_documents = memory;
    if (status == SXML_STATUS_OK) {
        memset(memory, 0, count * sizeof(sxml_schema_document));
        status = sxml_allocator_allocate(
            request->allocator,
            (sxml_allocation_request){.byte_count = uri_byte_count, .alignment = 1},
            &memory);
        documents->uri_bytes = memory;
    }
    size_t uri_offset = 0;
    for (size_t document_index = 0; status == SXML_STATUS_OK && document_index < count;
         document_index += 1) {
        const sxml_byte_view file_path = request->documents.file_paths[document_index];
        sxml_byte_view contents = sxml_xsd_test_empty_text;
        status = sxml_xsd_test_catalog_read_file(
            request->catalog,
            &(sxml_qt4_file_request){.suite_path = file_path,
                                     .allocator = request->allocator},
            &contents);
        if (status != SXML_STATUS_OK) {
            sxml_xsd_test_fail(answer,
                               (sxml_xsd_test_failure){.stage = SXML_TEXT("document"),
                                                       .subject = file_path,
                                                       .status = status});
            break;
        }
        uint8_t *uri = documents->uri_bytes + uri_offset;
        memcpy(uri, sxml_xsd_test_uri_prefix.bytes, sxml_xsd_test_uri_prefix.byte_count);
        memcpy(uri + sxml_xsd_test_uri_prefix.byte_count, file_path.bytes,
               file_path.byte_count);
        const size_t uri_count = sxml_xsd_test_uri_prefix.byte_count + file_path.byte_count;
        uri_offset += uri_count;
        documents->schema_documents[document_index] = (sxml_schema_document){
            .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
            .document_text = contents,
            .document_uri = {.bytes = uri, .byte_count = uri_count},
            .checks_target_namespace = false};
        documents->document_count = document_index + 1U;
    }
    if (status != SXML_STATUS_OK) {
        if (answer->outcome != SXML_QT4_TEST_OUTCOME_ERROR) {
            sxml_xsd_test_fail(answer,
                               (sxml_xsd_test_failure){.stage = SXML_TEXT("document"),
                                                       .subject = sxml_xsd_test_empty_text,
                                                       .status = status});
        }
        sxml_xsd_test_release_documents(request->allocator, documents);
        return false;
    }
    return true;
}

/* -------------------------------------------------------------- Reports */

/* Counts the errors a compilation or validation reports, and goes on. */
static sxml_status sxml_xsd_test_count_report(void *state,
                                              const sxml_schema_report *report) {
    (void)report;
    size_t *error_count = state;
    *error_count += 1;
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations sxml_xsd_test_report_operations = {
    .receive = sxml_xsd_test_count_report};

/* ------------------------------------------------------------- Running */

/* What compiling a test's schema gave. */
typedef struct sxml_xsd_test_compilation {
    sxml_schema *schema;
    sxml_status status;
    sxml_error_record error_record;
} sxml_xsd_test_compilation;

typedef struct sxml_xsd_test_compile_request {
    sxml_processor *processor;
    sxml_allocator allocator;
    const sxml_xsd_test_documents *documents;
    sxml_byte_view base_uri;
    /* The parsed instance whose schema location hints join the schema
       (include/sxml/schema.h, D278); the zero item for none. */
    sxml_item hint_node;
} sxml_xsd_test_compile_request;

static void sxml_xsd_test_compile(const sxml_xsd_test_compile_request *request,
                                  sxml_xsd_test_compilation *compilation) {
    size_t error_count = 0;
    memset(compilation, 0, sizeof(*compilation));
    compilation->status = sxml_schema_create(
        request->allocator,
        &(sxml_schema_configuration){
            .processor = request->processor,
            .schema_documents = {.schema_documents = request->documents->schema_documents,
                                 .schema_document_count = request->documents->document_count},
            .base_uri = request->base_uri,
            .report_sink = {.state = &error_count,
                            .operations = &sxml_xsd_test_report_operations},
            .maximum_content_model_state_count = 0,
            .error_record = &compilation->error_record,
            .schema_location_hint_node = request->hint_node},
        &compilation->schema);
}

/* Appends "CODE" of an error record to the reason. */
static void sxml_xsd_test_append_code(sxml_xsd_test_answer *answer,
                                      const sxml_error_record *error_record) {
    if (error_record->code.local_name.byte_count != 0) {
        sxml_qt4_reason_append(answer->reason, SXML_TEXT(" "));
        sxml_qt4_reason_append(answer->reason, error_record->code.local_name);
    }
}

/* The verdict on a schema that compiled (valid) or not (invalid). */
typedef struct sxml_xsd_test_verdict {
    sxml_xsd_test_validity expected;
    bool is_valid;
    sxml_byte_view stage;
    const sxml_error_record *error_record;
} sxml_xsd_test_verdict;

static void sxml_xsd_test_judge(sxml_xsd_test_answer *answer, sxml_xsd_test_verdict verdict) {
    const bool expects_valid = verdict.expected == SXML_XSD_TEST_VALIDITY_VALID;
    if (expects_valid == verdict.is_valid) {
        answer->outcome = SXML_QT4_TEST_OUTCOME_PASS;
        return;
    }
    answer->outcome = SXML_QT4_TEST_OUTCOME_FAIL;
    sxml_qt4_reason_append(answer->reason, verdict.stage);
    sxml_qt4_reason_append(answer->reason, expects_valid ? SXML_TEXT(": invalid, expected valid")
                                                         : SXML_TEXT(": valid, expected invalid"));
    if (!verdict.is_valid && verdict.error_record != NULL) {
        sxml_xsd_test_append_code(answer, verdict.error_record);
    }
}

/* -------------------------------------------------------- Location hints */

/*
 * An instance's xsi:schemaLocation and xsi:noNamespaceSchemaLocation hints
 * (XSD 1.1 Part 1, 4.3.2) are followed by the library
 * (sxml_schema_configuration.schema_location_hint_node, D278): the test's
 * schema documents are compiled again with the parsed instance, and that
 * schema is used when it compiles (xsd_test_run.h). An instance without a
 * hint attribute is not compiled again.
 */
typedef struct sxml_xsd_test_hint_request {
    sxml_processor *processor;
    sxml_allocator allocator;
    const sxml_xsd_test_documents *schema_documents;
    const sxml_schema_document *instance;
} sxml_xsd_test_hint_request;

static sxml_name_identifier sxml_xsd_test_xsi_name(sxml_processor *processor,
                                                   sxml_byte_view local_name) {
    sxml_name_identifier name = {.index = 0};
    (void)sxml_name_pool_find_name(
        sxml_processor_get_name_pool(processor),
        &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                 SXML_NAMESPACE_XML_SCHEMA_INSTANCE),
                             .local_name = local_name},
        &name);
    return name;
}

/* True when the parsed instance has a hint attribute. */
static bool sxml_xsd_test_has_hints(sxml_processor *processor, sxml_item instance_node) {
    const sxml_name_identifier schema_location =
        sxml_xsd_test_xsi_name(processor, SXML_TEXT("schemaLocation"));
    const sxml_name_identifier no_namespace_schema_location =
        sxml_xsd_test_xsi_name(processor, SXML_TEXT("noNamespaceSchemaLocation"));
    sxml_node_reference document = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_tree_description tree_description;
    if ((schema_location.index == 0 && no_namespace_schema_location.index == 0) ||
        sxml_item_get_node(instance_node, &document) != SXML_STATUS_OK ||
        sxml_tree_describe(document.tree, &tree_description) != SXML_STATUS_OK) {
        return false;
    }
    for (uint32_t node_index = 1; node_index < tree_description.node_count; node_index += 1) {
        sxml_node_reference node = document;
        node.node_index.index = node_index;
        sxml_node_description description;
        if (sxml_node_describe(node, &description) == SXML_STATUS_OK &&
            description.node_kind == SXML_NODE_KIND_ATTRIBUTE && description.node_name.index != 0 &&
            (description.node_name.index == schema_location.index ||
             description.node_name.index == no_namespace_schema_location.index)) {
            return true;
        }
    }
    return false;
}

/* Compiles the test's schema documents with the instance's hints and,
   when that schema compiles, validates against it instead
   (xsd_test_run.h); hints that break the schema are not followed. */
static void sxml_xsd_test_add_hints(const sxml_xsd_test_hint_request *request,
                                    sxml_xsd_test_compilation *compilation) {
    const sxml_byte_view uri = request->instance->document_uri;
    sxml_item instance_node = {.representation = 0};
    if (sxml_processor_parse_document(
            request->processor,
            &(sxml_xml_parse_request){.input = request->instance->document_text,
                                      .source_name = uri,
                                      .document_uri = uri,
                                      .base_uri = uri,
                                      .encoding_name = sxml_xsd_test_empty_text,
                                      .diagnostic = NULL},
            &instance_node) != SXML_STATUS_OK) {
        /* Validation reports an instance that does not parse. */
        return;
    }
    if (!sxml_xsd_test_has_hints(request->processor, instance_node)) {
        sxml_item_release(instance_node);
        return;
    }
    sxml_xsd_test_compilation hinted;
    sxml_xsd_test_compile(&(sxml_xsd_test_compile_request){.processor = request->processor,
                                                           .allocator = request->allocator,
                                                           .documents = request->schema_documents,
                                                           .base_uri = sxml_xsd_test_empty_text,
                                                           .hint_node = instance_node},
                          &hinted);
    sxml_item_release(instance_node);
    if (hinted.status != SXML_STATUS_OK) {
        sxml_schema_destroy(hinted.schema);
        return;
    }
    sxml_schema_destroy(compilation->schema);
    *compilation = hinted;
}

/* A status that is neither valid nor invalid: an error of the test. */
static bool sxml_xsd_test_is_verdict(sxml_status status) {
    return status == SXML_STATUS_OK || status == SXML_STATUS_STATIC_ERROR ||
           status == SXML_STATUS_DYNAMIC_ERROR;
}

typedef struct sxml_xsd_test_instance_request {
    sxml_processor *processor;
    sxml_allocator allocator;
    const sxml_schema *schema;
    const sxml_xsd_test_documents *instance;
    sxml_xsd_test_validity expected;
} sxml_xsd_test_instance_request;

/* Validates the instance document while parsing it (xsd_test_run.h). */
static void sxml_xsd_test_validate(const sxml_xsd_test_instance_request *request,
                                   sxml_xsd_test_answer *answer) {
    size_t error_count = 0;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_schema_validator *validator = NULL;
    sxml_status status = sxml_schema_validator_create(
        request->allocator,
        &(sxml_schema_validator_configuration){
            .schema = request->schema,
            .validation_mode = SXML_VALIDATION_MODE_STRICT,
            .governing_type = {.index = 0},
            .result_builder = {.state = NULL, .operations = NULL},
            .report_sink = {.state = &error_count,
                            .operations = &sxml_xsd_test_report_operations},
            .error_record = &error_record},
        &validator);
    sxml_xml_parser *parser = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            request->allocator,
            &(sxml_xml_parser_configuration){
                .allocator = request->allocator,
                .name_pool = sxml_processor_get_name_pool(request->processor),
                .limits = {.maximum_depth = 0,
                           .maximum_entity_expansion_byte_count =
                               SXML_XSD_TEST_RUN_MAXIMUM_EXPANSION_BYTE_COUNT}},
            &parser);
    }
    const sxml_schema_document *document = &request->instance->schema_documents[0];
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            parser,
            &(sxml_xml_parse_request){.input = document->document_text,
                                      .source_name = document->document_uri,
                                      .document_uri = document->document_uri,
                                      .base_uri = document->document_uri,
                                      .encoding_name = sxml_xsd_test_empty_text,
                                      .diagnostic = NULL},
            sxml_schema_validator_as_tree_builder(validator));
    }
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_get_outcome(validator, &outcome);
    }
    if (status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        /* A missing feature of an in-scope test is a failure, as the XSLT
           driver counts it: never "not run" (D178, D95). */
        answer->outcome = SXML_QT4_TEST_OUTCOME_FAIL;
        sxml_qt4_reason_append(answer->reason, SXML_TEXT("instance: unsupported feature"));
    } else if (sxml_xsd_test_is_verdict(status) || status == SXML_STATUS_MALFORMED_INPUT) {
        sxml_xsd_test_judge(answer,
                            (sxml_xsd_test_verdict){
                                .expected = request->expected,
                                .is_valid = status == SXML_STATUS_OK &&
                                            outcome.validity == SXML_VALIDITY_VALID,
                                .stage = SXML_TEXT("instance"),
                                .error_record = &error_record});
    } else {
        sxml_xsd_test_fail(answer, (sxml_xsd_test_failure){.stage = SXML_TEXT("validation"),
                                                           .subject = sxml_xsd_test_empty_text,
                                                           .status = status});
    }
    sxml_xml_parser_destroy(parser);
    sxml_schema_validator_destroy(validator);
}

typedef struct sxml_xsd_test_isolated_request {
    const sxml_xsd_test_run_request *run;
    sxml_xsd_test_validity expected;
} sxml_xsd_test_isolated_request;

/* The processor a test runs in and the allocator of its budget. */
typedef struct sxml_xsd_test_environment {
    sxml_processor *processor;
    sxml_allocator allocator;
} sxml_xsd_test_environment;

/* Compiles the test's schema and, for an instance test, validates. */
static void sxml_xsd_test_run_in(const sxml_xsd_test_isolated_request *request,
                                 const sxml_xsd_test_environment *environment,
                                 sxml_xsd_test_answer *answer) {
    const sxml_xsd_test_run_request *run = request->run;
    const sxml_xsd_test_case *test_case = run->test_case;
    sxml_processor *processor = environment->processor;
    const sxml_allocator allocator = environment->allocator;
    sxml_xsd_test_documents schema_documents;
    sxml_xsd_test_documents instance;
    memset(&instance, 0, sizeof(instance));
    if (!sxml_xsd_test_read_documents(
            &(sxml_xsd_test_document_request){.catalog = run->catalog,
                                              .allocator = allocator,
                                              .documents = test_case->schema_documents},
            &schema_documents, answer)) {
        return;
    }
    const bool is_instance_test = test_case->kind == SXML_XSD_TEST_KIND_INSTANCE;
    if (is_instance_test &&
        (!sxml_xsd_test_read_documents(
             &(sxml_xsd_test_document_request){.catalog = run->catalog,
                                               .allocator = allocator,
                                               .documents = test_case->documents},
             &instance, answer) ||
         instance.document_count != 1)) {
        if (answer->outcome != SXML_QT4_TEST_OUTCOME_ERROR) {
            sxml_xsd_test_fail(answer, (sxml_xsd_test_failure){.stage = SXML_TEXT("instance"),
                                                               .subject = sxml_xsd_test_empty_text,
                                                               .status = SXML_STATUS_INVALID_ARGUMENT});
        }
        sxml_xsd_test_release_documents(allocator, &instance);
        sxml_xsd_test_release_documents(allocator, &schema_documents);
        return;
    }
    sxml_xsd_test_compilation compilation;
    sxml_xsd_test_compile(&(sxml_xsd_test_compile_request){.processor = processor,
                                                           .allocator = allocator,
                                                           .documents = &schema_documents,
                                                           .base_uri = sxml_xsd_test_empty_text,
                                                           .hint_node = {.representation = 0}},
                          &compilation);
    if (is_instance_test && compilation.status == SXML_STATUS_OK) {
        sxml_xsd_test_add_hints(&(sxml_xsd_test_hint_request){.processor = processor,
                                                              .allocator = allocator,
                                                              .schema_documents = &schema_documents,
                                                              .instance =
                                                                  &instance.schema_documents[0]},
                                &compilation);
    }
    if (!sxml_xsd_test_is_verdict(compilation.status)) {
        sxml_xsd_test_fail(answer, (sxml_xsd_test_failure){.stage = SXML_TEXT("schema"),
                                                           .subject = sxml_xsd_test_empty_text,
                                                           .status = compilation.status});
    } else if (!is_instance_test) {
        sxml_xsd_test_judge(answer, (sxml_xsd_test_verdict){
                                        .expected = request->expected,
                                        .is_valid = compilation.status == SXML_STATUS_OK,
                                        .stage = SXML_TEXT("schema"),
                                        .error_record = &compilation.error_record});
    } else if (compilation.status != SXML_STATUS_OK) {
        /* xsd_test_run.h: a group whose schema fails fails its instance
           tests. */
        answer->outcome = SXML_QT4_TEST_OUTCOME_FAIL;
        sxml_qt4_reason_append(answer->reason, SXML_TEXT("schema: invalid"));
        sxml_xsd_test_append_code(answer, &compilation.error_record);
    } else {
        sxml_xsd_test_validate(&(sxml_xsd_test_instance_request){.processor = processor,
                                                                 .allocator = allocator,
                                                                 .schema = compilation.schema,
                                                                 .instance = &instance,
                                                                 .expected = request->expected},
                               answer);
    }
    sxml_schema_destroy(compilation.schema);
    sxml_xsd_test_release_documents(allocator, &instance);
    sxml_xsd_test_release_documents(allocator, &schema_documents);
}

/* Runs an applicable test in its own processor and budget. */
static void sxml_xsd_test_run_isolated(const sxml_xsd_test_isolated_request *request,
                                       sxml_xsd_test_answer *answer) {
    sxml_qt4_budget budget = sxml_qt4_budget_start(request->run->allocator);
    const sxml_allocator allocator = sxml_qt4_budget_allocator(&budget);
    sxml_xsd_test_resolver_state resolver_state = {.catalog = request->run->catalog,
                                                   .allocator = allocator};
    sxml_processor *processor = NULL;
    const sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = {.state = &resolver_state,
                                  .operations = &sxml_xsd_test_resolver_operations},
            .limits = {.xml_limits =
                           {.maximum_depth = 0,
                            .maximum_entity_expansion_byte_count =
                                SXML_XSD_TEST_RUN_MAXIMUM_EXPANSION_BYTE_COUNT},
                       .maximum_evaluation_step_count =
                           SXML_QT4_TEST_MAXIMUM_STEP_COUNT},
            .optional_features = sxml_all_optional_features()},
        &processor);
    if (status != SXML_STATUS_OK) {
        sxml_xsd_test_fail(
            answer, (sxml_xsd_test_failure){.stage = SXML_TEXT("processor"),
                                            .subject = sxml_xsd_test_empty_text,
                                            .status = status});
    } else {
        sxml_xsd_test_run_in(
            request,
            &(sxml_xsd_test_environment){.processor = processor, .allocator = allocator},
            answer);
    }
    sxml_processor_destroy(processor);
    sxml_qt4_budget_finish(
        &budget, (sxml_qt4_budget_verdict){.outcome = &answer->outcome,
                                           .reason = answer->reason});
}

sxml_status
sxml_xsd_test_run_test_case(const sxml_xsd_test_run_request *request,
                            sxml_qt4_test_result *result) {
    sxml_qt4_reason reason = {
        .bytes = NULL, .byte_count = 0, .capacity = 0, .is_truncated = false};
    const sxml_status status = sxml_qt4_reason_start(request->arena, &reason);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xsd_test_answer answer = {.outcome = SXML_QT4_TEST_OUTCOME_FAIL,
                                   .reason = &reason};
    sxml_xsd_test_decision decision;
    sxml_xsd_test_decide(request->test_case, &decision);
    if (!decision.applicability.is_applicable) {
        answer.outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
        sxml_qt4_reason_append(&reason, SXML_TEXT("skip: "));
        sxml_qt4_reason_append(&reason, decision.applicability.reason);
        if (decision.applicability.detail.byte_count != 0) {
            sxml_qt4_reason_append(&reason, SXML_TEXT(" "));
            sxml_qt4_reason_append(&reason, decision.applicability.detail);
        }
    } else {
        sxml_xsd_test_run_isolated(
            &(sxml_xsd_test_isolated_request){.run = request,
                                              .expected = decision.expected_validity},
            &answer);
    }
    *result = (sxml_qt4_test_result){
        .outcome = answer.outcome,
        .reason = answer.outcome == SXML_QT4_TEST_OUTCOME_PASS
                      ? sxml_xsd_test_empty_text
                      : sxml_qt4_reason_text(&reason),
        .is_in_scope = decision.applicability.is_in_scope};
    return SXML_STATUS_OK;
}
