/*
 * Unit tests for src/schema.c: the sxml_schema quintet, the find
 * functions and compilation through include/sxml/schema.h (D165, D170):
 * documents given as text, through the resolver and as nodes, an
 * instance's schema location hints (D278), the error record and the
 * report sink.
 *
 * Golden file: tests/xslt_test.c (stylesheet_lifecycle_checks_its_arguments:
 * a public object's lifecycle on caller memory and through an allocator).
 */
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const char order_schema[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema' targetNamespace='urn:o' "
    "xmlns:o='urn:o'><xs:element name='order' type='xs:string' nillable='true'/>"
    "<xs:element name='base' abstract='true'/>"
    "<xs:attribute name='code' type='xs:int'/></xs:schema>";

static sxml_processor *open_processor(const sxml_resource_entry_view *entries) {
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_processor *processor = NULL;
    if (sxml_processor_create(
            allocator,
            &(sxml_processor_configuration){
                .allocator = allocator,
                .resource_resolver =
                    entries == NULL ? (sxml_resource_resolver){.state = NULL, .operations = NULL}
                                    : sxml_table_resource_resolver(entries)},
            &processor) != SXML_STATUS_OK) {
        return NULL;
    }
    return processor;
}

static sxml_name_identifier name_in(sxml_processor *processor, const char *namespace_uri,
                                    const char *local_name) {
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(processor);
    sxml_namespace_uri_identifier uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_namespace_uri(name_pool, sxml_byte_view_from_c_string(namespace_uri),
                                            &uri) != SXML_STATUS_OK ||
        sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = uri,
                                 .local_name = sxml_byte_view_from_c_string(local_name)},
            &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

static sxml_schema_document text_document(const char *text) {
    return (sxml_schema_document){.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                  .document_text = sxml_byte_view_from_c_string(text),
                                  .document_uri = TEXT("file:///o.xsd")};
}

static void test_lifecycle_checks_its_arguments(sxml_test_state *state) {
    sxml_processor *processor = open_processor(NULL);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(
        state, sxml_schema_memory_requirements(&(sxml_schema_configuration){.processor = NULL},
                                               &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state, sxml_schema_memory_requirements(
                   &(sxml_schema_configuration){
                       .processor = processor,
                       .schema_documents = {.schema_documents = NULL, .schema_document_count = 1}},
                   &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_schema_configuration empty = {.processor = processor};
    SXML_TEST_REQUIRE(state,
                      sxml_schema_memory_requirements(&empty, &requirements) == SXML_STATUS_OK);
    alignas(max_align_t) uint8_t memory[4096];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(memory));
    sxml_schema *schema = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_schema_initialize((sxml_memory_view){.bytes = memory, .byte_count = 1},
                                            &empty, &schema) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(
        state, sxml_schema_initialize(
                   (sxml_memory_view){.bytes = memory + 1, .byte_count = requirements.byte_count},
                   &empty, &schema) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, schema == NULL);
    /* No documents: the schema of the built-in components. */
    SXML_TEST_REQUIRE(
        state, sxml_schema_initialize(
                   (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
                   &empty, &schema) == SXML_STATUS_OK);
    sxml_type_identifier type = {.index = 0};
    SXML_TEST_EXPECT(
        state, sxml_schema_find_type(
                   schema, name_in(processor, "http://www.w3.org/2001/XMLSchema", "int"), &type));
    SXML_TEST_EXPECT(state, type.index == (uint32_t)SXML_TYPE_XS_INT);
    SXML_TEST_EXPECT(
        state,
        !sxml_schema_find_type(
            schema, name_in(processor, "http://www.w3.org/2001/XMLSchema", "untyped"), &type));
    sxml_attribute_declaration_description attribute;
    SXML_TEST_EXPECT(
        state, sxml_schema_find_attribute_declaration(
                   schema, name_in(processor, "http://www.w3.org/2001/XMLSchema-instance", "nil"),
                   &attribute));
    SXML_TEST_EXPECT(state, attribute.declared_type.index == (uint32_t)SXML_TYPE_XS_BOOLEAN);
    sxml_schema_finalize(schema);
    sxml_schema_finalize(NULL);
    sxml_schema_destroy(NULL);
    SXML_TEST_EXPECT(state, sxml_schema_create(sxml_default_allocator(), NULL, &schema) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_processor_destroy(processor);
}

static void test_declarations_found(sxml_test_state *state) {
    sxml_processor *processor = open_processor(NULL);
    SXML_TEST_REQUIRE(state, processor != NULL);
    const sxml_schema_document document = text_document(order_schema);
    sxml_schema *schema = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_schema_create(
                   sxml_default_allocator(),
                   &(sxml_schema_configuration){.processor = processor,
                                                .schema_documents = {.schema_documents = &document,
                                                                     .schema_document_count = 1}},
                   &schema) == SXML_STATUS_OK);
    sxml_element_declaration_description element;
    SXML_TEST_EXPECT(state, sxml_schema_find_element_declaration(
                                schema, name_in(processor, "urn:o", "order"), &element));
    SXML_TEST_EXPECT(state, element.declared_type.index == (uint32_t)SXML_TYPE_XS_STRING);
    SXML_TEST_EXPECT(state, element.is_nillable && !element.is_abstract);
    SXML_TEST_EXPECT(state, sxml_schema_find_element_declaration(
                                schema, name_in(processor, "urn:o", "base"), &element));
    SXML_TEST_EXPECT(state, element.is_abstract);
    SXML_TEST_EXPECT(state, element.declared_type.index == (uint32_t)SXML_TYPE_XS_ANY_TYPE);
    SXML_TEST_EXPECT(state, !sxml_schema_find_element_declaration(
                                schema, name_in(processor, "", "order"), &element));
    sxml_attribute_declaration_description attribute;
    SXML_TEST_EXPECT(state, sxml_schema_find_attribute_declaration(
                                schema, name_in(processor, "urn:o", "code"), &attribute));
    SXML_TEST_EXPECT(state, attribute.declared_type.index == (uint32_t)SXML_TYPE_XS_INT);
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* RESOLVED documents through the resolver, and the target namespace a
   caller requires (XQuery import schema). */
static void test_resolved_documents(sxml_test_state *state) {
    const sxml_resource_entry entries[] = {
        {.uri = TEXT("file:///s/o.xsd"), .content = sxml_byte_view_from_c_string(order_schema)}};
    const sxml_resource_entry_view entry_view = {.resource_entries = entries,
                                                 .resource_entry_count = 1};
    sxml_processor *processor = open_processor(&entry_view);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_schema_document document = {.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED,
                                     .document_uri = TEXT("o.xsd"),
                                     .checks_target_namespace = true,
                                     .target_namespace_uri = TEXT("urn:o")};
    sxml_schema_configuration configuration = {
        .processor = processor,
        .schema_documents = {.schema_documents = &document, .schema_document_count = 1},
        .base_uri = TEXT("file:///s/")};
    sxml_schema *schema = NULL;
    SXML_TEST_EXPECT(state, sxml_schema_create(sxml_default_allocator(), &configuration, &schema) ==
                                SXML_STATUS_OK);
    sxml_schema_destroy(schema);
    schema = NULL;
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    configuration.error_record = &record;
    document.target_namespace_uri = TEXT("urn:other");
    SXML_TEST_EXPECT(state, sxml_schema_create(sxml_default_allocator(), &configuration, &schema) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(record.code.local_name, TEXT("src-import")));
    document.document_uri = TEXT("missing.xsd");
    SXML_TEST_EXPECT(state, sxml_schema_create(sxml_default_allocator(), &configuration, &schema) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, schema == NULL);
    sxml_processor_destroy(processor);
}

/* An import of the xml: namespace that the resolver does not deliver
   takes the document of the namespace already there, not the built-in
   one as well (Part 1, 4.2.6.2; D180). */
static void test_xml_namespace_document_kept(sxml_test_state *state) {
    sxml_processor *processor = open_processor(NULL);
    SXML_TEST_REQUIRE(state, processor != NULL);
    const sxml_schema_document documents[] = {
        {.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
         .document_text = TEXT("<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema' "
                               "targetNamespace='http://www.w3.org/XML/1998/namespace'>"
                               "<xs:attribute name='lang' type='xs:language'/></xs:schema>"),
         .document_uri = TEXT("file:///xml.xsd")},
        {.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
         .document_text = TEXT("<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                               "<xs:import namespace='http://www.w3.org/XML/1998/namespace' "
                               "schemaLocation='http://www.w3.org/2001/xml.xsd'/>"
                               "<xs:element name='e'><xs:complexType>"
                               "<xs:attribute ref='xml:lang'/></xs:complexType></xs:element>"
                               "</xs:schema>"),
         .document_uri = TEXT("file:///e.xsd")}};
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    const sxml_schema_configuration configuration = {
        .processor = processor,
        .schema_documents = {.schema_documents = documents, .schema_document_count = 2},
        .error_record = &record};
    sxml_schema *schema = NULL;
    SXML_TEST_EXPECT(state, sxml_schema_create(sxml_default_allocator(), &configuration, &schema) ==
                                SXML_STATUS_OK);
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* A NODE document: a parsed document node, read where it is. */
static void test_node_documents(sxml_test_state *state) {
    sxml_processor *processor = open_processor(NULL);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_item document_node = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, sxml_processor_parse_document(
                   processor,
                   &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(order_schema),
                                             .document_uri = TEXT("file:///o.xsd"),
                                             .base_uri = TEXT("file:///o.xsd")},
                   &document_node) == SXML_STATUS_OK);
    const sxml_schema_document document = {.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_NODE,
                                           .document_node = document_node,
                                           .document_uri = TEXT("file:///o.xsd")};
    sxml_schema *schema = NULL;
    SXML_TEST_EXPECT(
        state, sxml_schema_create(
                   sxml_default_allocator(),
                   &(sxml_schema_configuration){.processor = processor,
                                                .schema_documents = {.schema_documents = &document,
                                                                     .schema_document_count = 1}},
                   &schema) == SXML_STATUS_OK);
    sxml_element_declaration_description element;
    SXML_TEST_EXPECT(state, sxml_schema_find_element_declaration(
                                schema, name_in(processor, "urn:o", "order"), &element));
    sxml_schema_destroy(schema);
    sxml_item_release(document_node);
    sxml_processor_destroy(processor);
}

/* An instance to take schema location hints from (D278). */
static sxml_item parse_instance(sxml_processor *processor, const char *text) {
    sxml_item document_node = {.representation = 0};
    if (sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(text),
                                      .document_uri = TEXT("file:///s/i.xml"),
                                      .base_uri = TEXT("file:///s/i.xml")},
            &document_node) != SXML_STATUS_OK) {
        return (sxml_item){.representation = 0};
    }
    return document_node;
}

/* Compiles the configured documents with the hints of instance; the
   status, and *schema when it compiles. */
typedef struct hinted_compilation {
    sxml_processor *processor;
    const sxml_schema_document *documents;
    size_t document_count;
    sxml_item instance;
    sxml_error_record *error_record;
} hinted_compilation;

static sxml_status compile_with_hints(const hinted_compilation *compilation,
                                      sxml_schema **schema) {
    return sxml_schema_create(
        sxml_default_allocator(),
        &(sxml_schema_configuration){
            .processor = compilation->processor,
            .schema_documents = {.schema_documents = compilation->documents,
                                 .schema_document_count = compilation->document_count},
            .error_record = compilation->error_record,
            .schema_location_hint_node = compilation->instance},
        schema);
}

/* Schema location hints (XSD 1.1 Part 1, 4.3.2; D278): followed only when
   the configuration names the instance, on any element, relative to the
   element's base URI; a missing document is no error; a namespace the
   configured documents have already is not read again; the hinted
   document must have the hinted namespace. */
static void test_location_hints(sxml_test_state *state) {
    static const char no_namespace_schema[] =
        "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
        "<xs:element name='plain' type='xs:string'/></xs:schema>";
    static const char other_order_schema[] =
        "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema' targetNamespace='urn:o'>"
        "<xs:element name='other' type='xs:string'/></xs:schema>";
    const sxml_resource_entry entries[] = {
        {.uri = TEXT("file:///s/o.xsd"), .content = sxml_byte_view_from_c_string(order_schema)},
        {.uri = TEXT("file:///s/sub/n.xsd"),
         .content = sxml_byte_view_from_c_string(no_namespace_schema)},
        {.uri = TEXT("file:///s/other.xsd"),
         .content = sxml_byte_view_from_c_string(other_order_schema)}};
    const sxml_resource_entry_view entry_view = {.resource_entries = entries,
                                                 .resource_entry_count = 3};
    sxml_processor *processor = open_processor(&entry_view);
    SXML_TEST_REQUIRE(state, processor != NULL);
    const sxml_item instance = parse_instance(
        processor,
        "<root xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
        "xsi:schemaLocation='urn:o o.xsd urn:missing missing.xsd'>"
        "<x xml:base='sub/' xsi:noNamespaceSchemaLocation='n.xsd'/></root>");
    SXML_TEST_REQUIRE(state, instance.representation != 0);
    const sxml_name_identifier order = name_in(processor, "urn:o", "order");
    const sxml_name_identifier plain = name_in(processor, "", "plain");
    sxml_element_declaration_description element;
    hinted_compilation compilation = {.processor = processor,
                                      .documents = NULL,
                                      .document_count = 0,
                                      .instance = instance,
                                      .error_record = NULL};
    sxml_schema *schema = NULL;
    SXML_TEST_EXPECT(state, compile_with_hints(&compilation, &schema) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_schema_find_element_declaration(schema, order, &element));
    SXML_TEST_EXPECT(state, sxml_schema_find_element_declaration(schema, plain, &element));
    sxml_schema_destroy(schema);
    schema = NULL;
    /* The zero item: today's behaviour, no hint followed. */
    compilation.instance = (sxml_item){.representation = 0};
    SXML_TEST_EXPECT(state, compile_with_hints(&compilation, &schema) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !sxml_schema_find_element_declaration(schema, order, &element));
    sxml_schema_destroy(schema);
    schema = NULL;
    /* The configured schema has urn:o: the hint for it is ignored. */
    const sxml_schema_document configured = {
        .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
        .document_text = sxml_byte_view_from_c_string(other_order_schema),
        .document_uri = TEXT("file:///s/other.xsd")};
    compilation.documents = &configured;
    compilation.document_count = 1;
    compilation.instance = instance;
    SXML_TEST_EXPECT(state, compile_with_hints(&compilation, &schema) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !sxml_schema_find_element_declaration(schema, order, &element));
    SXML_TEST_EXPECT(state, sxml_schema_find_element_declaration(schema, plain, &element));
    sxml_schema_destroy(schema);
    schema = NULL;
    sxml_item_release(instance);
    /* A hinted document of another namespace than its hint's. */
    const sxml_item wrong = parse_instance(
        processor, "<root xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                   "xsi:schemaLocation='urn:y o.xsd'/>");
    SXML_TEST_REQUIRE(state, wrong.representation != 0);
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    compilation = (hinted_compilation){.processor = processor,
                                       .documents = NULL,
                                       .document_count = 0,
                                       .instance = wrong,
                                       .error_record = &record};
    SXML_TEST_EXPECT(state, compile_with_hints(&compilation, &schema) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, schema == NULL);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(record.code.local_name, TEXT("schema_reference")));
    sxml_item_release(wrong);
    sxml_processor_destroy(processor);
}

static sxml_status count_report(void *state, const sxml_schema_report *report) {
    size_t *count = state;
    *count += report->location.line_number != 0 ? 1U : 0U;
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations counting_sink = {.receive = count_report};

/* The error record has the first error; the sink receives every one, with
   locations; the null sink stops at the first. */
static void test_errors_reported(sxml_test_state *state) {
    sxml_processor *processor = open_processor(NULL);
    SXML_TEST_REQUIRE(state, processor != NULL);
    const sxml_schema_document document =
        text_document("<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>\n"
                      "<xs:element name='a' type='x'/>\n<xs:element name='b' type='y'/>\n"
                      "</xs:schema>");
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    size_t error_count = 0;
    sxml_schema *schema = NULL;
    SXML_TEST_EXPECT(
        state,
        sxml_schema_create(
            sxml_default_allocator(),
            &(sxml_schema_configuration){
                .processor = processor,
                .schema_documents = {.schema_documents = &document, .schema_document_count = 1},
                .report_sink = {.state = &error_count, .operations = &counting_sink},
                .error_record = &record},
            &schema) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_count == 2);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(record.code.local_name, TEXT("src-resolve")));
    SXML_TEST_EXPECT(state, record.code.namespace_uri.byte_count == 0);
    SXML_TEST_EXPECT(state, record.diagnostic.location.line_number == 2);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(record.diagnostic.location.source_name,
                                                  TEXT("file:///o.xsd")));
    const sxml_schema_document broken = text_document("<xs:schema");
    SXML_TEST_EXPECT(state, sxml_schema_create(sxml_default_allocator(),
                                               &(sxml_schema_configuration){
                                                   .processor = processor,
                                                   .schema_documents = {.schema_documents = &broken,
                                                                        .schema_document_count = 1},
                                                   .error_record = &record},
                                               &schema) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(record.code.local_name, TEXT("schema_reference")));
    sxml_processor_destroy(processor);
}

/* Compiles one document; writes the first error's code (empty when it
   compiles) to *code, which borrows from the record. */
typedef struct compilation_result {
    sxml_status status;
    sxml_byte_view code;
} compilation_result;

static compilation_result compile_text(sxml_processor *processor, const char *text) {
    static sxml_error_record record;
    memset(&record, 0, sizeof(record));
    const sxml_schema_document document = text_document(text);
    sxml_schema *schema = NULL;
    const sxml_status status = sxml_schema_create(
        sxml_default_allocator(),
        &(sxml_schema_configuration){
            .processor = processor,
            .schema_documents = {.schema_documents = &document, .schema_document_count = 1},
            .error_record = &record},
        &schema);
    sxml_schema_destroy(schema);
    return (compilation_result){.status = status,
                                .code = status == SXML_STATUS_OK
                                            ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                            : record.code.local_name};
}

#define XS "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"

/* Component rules of XSD 1.1 (packet 5.13 sweep 3). */
static void test_component_rules(sxml_test_state *state) {
    sxml_processor *processor = open_processor(NULL);
    SXML_TEST_REQUIRE(state, processor != NULL);
    const struct {
        const char *document;
        const char *code;
    } cases[] = {
        /* 3.4.2.2: simple content restricting xs:anySimpleType. */
        {XS "<xs:complexType name='t'><xs:simpleContent><xs:restriction base='xs:anyType'/>"
            "</xs:simpleContent></xs:complexType></xs:schema>",
         "cos-st-restricts.1.1"},
        /* A required attribute of the base may not be prohibited. */
        {XS "<xs:complexType name='b'><xs:attribute name='a' use='required'/></xs:complexType>"
            "<xs:complexType name='r'><xs:complexContent><xs:restriction base='b'>"
            "<xs:attribute name='a' use='prohibited'/></xs:restriction></xs:complexContent>"
            "</xs:complexType></xs:schema>",
         "derivation-ok-restriction.3"},
        /* Fixed values compare as values (equal or identical). */
        {XS "<xs:complexType name='b'><xs:attribute name='a' type='xs:int' fixed=' 01'/>"
            "</xs:complexType><xs:complexType name='r'><xs:complexContent>"
            "<xs:restriction base='b'><xs:attribute name='a' type='xs:int' fixed='1'/>"
            "</xs:restriction></xs:complexContent></xs:complexType></xs:schema>",
         ""},
        /* 3.8.6.3: a wildcard competes with a local declaration whose
           type table differs from the top-level one's. */
        {XS "<xs:element name='a'><xs:alternative type='xs:integer'/></xs:element>"
            "<xs:complexType name='t'><xs:sequence><xs:element name='a'/>"
            "<xs:any namespace='##local'/></xs:sequence></xs:complexType></xs:schema>",
         "cos-element-consistent"},
        /* A wildcard that only the union of the base's two covers. */
        {XS "<xs:complexType name='b'><xs:choice maxOccurs='2'>"
            "<xs:any namespace='##local' processContents='skip'/>"
            "<xs:any namespace='##other' processContents='skip'/></xs:choice></xs:complexType>"
            "<xs:complexType name='r'><xs:complexContent><xs:restriction base='b'>"
            "<xs:sequence><xs:any namespace='##any' processContents='skip'/></xs:sequence>"
            "</xs:restriction></xs:complexContent></xs:complexType></xs:schema>",
         ""},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        const compilation_result result = compile_text(processor, cases[case_index].document);
        const sxml_byte_view expected = sxml_byte_view_from_c_string(cases[case_index].code);
        const bool matches = sxml_byte_view_equals(result.code, expected);
        if (!matches) {
            (void)fprintf(stderr, "case %zu: got '%.*s'\n", case_index,
                          (int)result.code.byte_count, (const char *)result.code.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
    }
    sxml_processor_destroy(processor);
}

static const sxml_test_case schema_test_cases[] = {
    {.name = "lifecycle_checks_its_arguments", .function = test_lifecycle_checks_its_arguments},
    {.name = "declarations_found", .function = test_declarations_found},
    {.name = "resolved_documents", .function = test_resolved_documents},
    {.name = "xml_namespace_document_kept", .function = test_xml_namespace_document_kept},
    {.name = "node_documents", .function = test_node_documents},
    {.name = "location_hints", .function = test_location_hints},
    {.name = "errors_reported", .function = test_errors_reported},
    {.name = "component_rules", .function = test_component_rules},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema",
            .cases = schema_test_cases,
            .case_count = SXML_TEST_COUNT(schema_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
