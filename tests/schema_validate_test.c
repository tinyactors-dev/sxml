/*
 * Unit tests for the schema validator (src/schema_validate.c,
 * src/schema_validate_value.c; include/sxml/schema.h; D165, D173): the
 * validator's lifecycle, validation while parsing with a report sink
 * (content models, attributes, xsi:type, xsi:nil, value constraints,
 * wildcards, ID and IDREF, lax and strict modes), the annotated events it
 * passes on, and sxml_schema_validate_node with typed values derived
 * from the annotations (fn:data of a validated element); and an
 * allocation-failure loop over compilation, validation while parsing and
 * sxml_schema_validate_node (D23, the Phase 5 review).
 *
 * Golden file: tests/schema_test.c (a schema compiled from text through
 * the public API, the error record and the report sink).
 */
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

#include <stdalign.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The purchase order of src/schema_internal.h, example 1, without its key
   (identity constraints are packet 5.8's), with a few more cases. */
static const char order_schema[] =
    "<xsd:schema xmlns:xsd='http://www.w3.org/2001/XMLSchema'>"
    "<xsd:element name='purchaseOrder' type='PurchaseOrderType'/>"
    "<xsd:element name='comment' type='xsd:string'/>"
    "<xsd:complexType name='PurchaseOrderType'><xsd:sequence>"
    "<xsd:element name='shipTo' type='USAddress'/>"
    "<xsd:element name='billTo' type='USAddress'/>"
    "<xsd:element ref='comment' minOccurs='0'/>"
    "<xsd:element name='items' type='Items'/></xsd:sequence>"
    "<xsd:attribute name='orderDate' type='xsd:date'/></xsd:complexType>"
    "<xsd:complexType name='USAddress'><xsd:sequence>"
    "<xsd:element name='name' type='xsd:string'/>"
    "<xsd:element name='zip' type='xsd:decimal'/></xsd:sequence>"
    "<xsd:attribute name='country' type='xsd:NMTOKEN' fixed='US'/></xsd:complexType>"
    "<xsd:complexType name='Items'><xsd:sequence>"
    "<xsd:element name='item' minOccurs='0' maxOccurs='unbounded'><xsd:complexType>"
    "<xsd:sequence><xsd:element name='productName' type='xsd:string'/>"
    "<xsd:element name='quantity'><xsd:simpleType>"
    "<xsd:restriction base='xsd:positiveInteger'><xsd:maxExclusive value='100'/>"
    "</xsd:restriction></xsd:simpleType></xsd:element></xsd:sequence>"
    "<xsd:attribute name='partNum' type='SKU' use='required'/>"
    "</xsd:complexType></xsd:element></xsd:sequence></xsd:complexType>"
    "<xsd:simpleType name='SKU'><xsd:restriction base='xsd:string'>"
    "<xsd:pattern value='\\d{3}-[A-Z]{2}'/></xsd:restriction></xsd:simpleType>"
    "</xsd:schema>";

static const char order_document[] =
    "<purchaseOrder orderDate='1999-10-20'>"
    "<shipTo country='US'><name>Alice Smith</name><zip>90952</zip></shipTo>"
    "<billTo><name>Robert Smith</name><zip>95819</zip></billTo>"
    "<comment>Hurry</comment>"
    "<items><item partNum='872-AA'><productName>Lawnmower</productName>"
    "<quantity>1</quantity></item></items></purchaseOrder>";

/* Declarations for the other cases. */
static const char misc_schema[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
    "<xs:simpleType name='Big'><xs:restriction base='xs:integer'>"
    "<xs:minInclusive value='10'/></xs:restriction></xs:simpleType>"
    "<xs:element name='a' type='Big'/>"
    "<xs:element name='n' type='xs:int' nillable='true'/>"
    "<xs:element name='d' type='xs:int' default='7'/>"
    "<xs:element name='f' type='xs:decimal' fixed='1.5'/>"
    "<xs:complexType name='Base'><xs:sequence>"
    "<xs:element name='x' type='xs:string'/></xs:sequence></xs:complexType>"
    "<xs:complexType name='Derived'><xs:complexContent><xs:extension base='Base'>"
    "<xs:sequence><xs:element name='y' type='xs:string'/></xs:sequence>"
    "</xs:extension></xs:complexContent></xs:complexType>"
    "<xs:element name='b' type='Base'/>"
    "<xs:element name='e'><xs:complexType><xs:sequence>"
    "<xs:any processContents='skip' minOccurs='0'/></xs:sequence>"
    "<xs:attribute name='k' type='xs:int' default='3'/>"
    "</xs:complexType></xs:element>"
    "<xs:element name='ids'><xs:complexType><xs:sequence>"
    "<xs:element name='i' maxOccurs='unbounded'><xs:complexType>"
    "<xs:attribute name='id' type='xs:ID'/><xs:attribute name='id2' type='xs:ID'/>"
    "<xs:attribute name='ref' type='xs:IDREFS'/>"
    "</xs:complexType></xs:element></xs:sequence></xs:complexType></xs:element>"
    "<xs:element name='empty'><xs:complexType/></xs:element>"
    "</xs:schema>";

static sxml_processor *open_processor(void) {
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_processor *processor = NULL;
    if (sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                              &processor) != SXML_STATUS_OK) {
        return NULL;
    }
    return processor;
}

static sxml_schema *compile(sxml_processor *processor, const char *text) {
    sxml_schema *schema = NULL;
    if (sxml_schema_create(
            sxml_default_allocator(),
            &(sxml_schema_configuration){
                .processor = processor,
                .schema_documents = {.schema_documents =
                                         &(sxml_schema_document){
                                             .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                             .document_text = sxml_byte_view_from_c_string(text),
                                             .document_uri = TEXT("file:///test.xsd")},
                                     .schema_document_count = 1}},
            &schema) != SXML_STATUS_OK) {
        return NULL;
    }
    return schema;
}

/* Every report a validation sends, the first one's code kept. */
typedef struct report_log {
    size_t report_count;
    char first_code[64];
} report_log;

static sxml_status log_report(void *state, const sxml_schema_report *report) {
    report_log *log = state;
    if (log->report_count == 0) {
        const size_t byte_count = report->code.local_name.byte_count < sizeof(log->first_code) - 1
                                      ? report->code.local_name.byte_count
                                      : sizeof(log->first_code) - 1;
        memcpy(log->first_code, report->code.local_name.bytes, byte_count);
        log->first_code[byte_count] = '\0';
    }
    log->report_count += 1;
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations log_operations = {.receive = log_report};

/* What validating a document while parsing it gave. */
typedef struct validation_result {
    sxml_status status;
    sxml_validation_outcome outcome;
    report_log log;
} validation_result;

typedef struct validation_request {
    sxml_processor *processor;
    const sxml_schema *schema;
    const char *document;
    sxml_validation_mode validation_mode;
} validation_request;

static validation_result validate(const validation_request *request) {
    validation_result result = {.status = SXML_STATUS_OK,
                                .outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0},
                                .log = {.report_count = 0, .first_code = {0}}};
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_schema_validator *validator = NULL;
    result.status = sxml_schema_validator_create(
        allocator,
        &(sxml_schema_validator_configuration){
            .schema = request->schema,
            .validation_mode = request->validation_mode,
            .governing_type = {.index = SXML_TYPE_NONE},
            .result_builder = {.state = NULL, .operations = NULL},
            .report_sink = {.state = &result.log, .operations = &log_operations},
            .error_record = NULL},
        &validator);
    sxml_xml_parser *parser = NULL;
    if (result.status == SXML_STATUS_OK) {
        result.status = sxml_xml_parser_create(
            allocator,
            &(sxml_xml_parser_configuration){.allocator = allocator,
                                             .name_pool =
                                                 sxml_processor_get_name_pool(request->processor)},
            &parser);
    }
    if (result.status == SXML_STATUS_OK) {
        result.status = sxml_xml_parser_parse(
            parser,
            &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(request->document),
                                      .source_name = TEXT("test.xml")},
            sxml_schema_validator_as_tree_builder(validator));
    }
    if (result.status == SXML_STATUS_OK) {
        result.status = sxml_schema_validator_get_outcome(validator, &result.outcome);
    }
    sxml_xml_parser_destroy(parser);
    sxml_schema_validator_destroy(validator);
    return result;
}

/* Validates strictly and checks the validity and the first report. */
typedef struct expectation {
    const char *document;
    sxml_validity validity;
    /* The first report's constraint name; NULL for none. */
    const char *first_code;
} expectation;

/* The processor and schema a test validates with. */
typedef struct test_schema {
    sxml_processor *processor;
    const sxml_schema *schema;
} test_schema;

static void expect_validity(sxml_test_state *state, test_schema tested, expectation expected) {
    sxml_processor *processor = tested.processor;
    const sxml_schema *schema = tested.schema;
    const validation_result result =
        validate(&(validation_request){.processor = processor,
                                       .schema = schema,
                                       .document = expected.document,
                                       .validation_mode = SXML_VALIDATION_MODE_STRICT});
    SXML_TEST_EXPECT(state, result.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, result.outcome.validity == expected.validity);
    if (expected.first_code == NULL) {
        SXML_TEST_EXPECT(state, result.log.report_count == 0);
    } else {
        SXML_TEST_EXPECT(state, strcmp(result.log.first_code, expected.first_code) == 0);
    }
}

static void test_lifecycle_checks_its_arguments(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_schema *schema = compile(processor, misc_schema);
    SXML_TEST_REQUIRE(state, schema != NULL);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_schema_validator_memory_requirements(
                                &(sxml_schema_validator_configuration){.schema = NULL},
                                &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_schema_validator_memory_requirements(
                                &(sxml_schema_validator_configuration){
                                    .schema = schema, .governing_type = {.index = 1000000}},
                                &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_schema_validator_configuration configuration = {.schema = schema};
    SXML_TEST_REQUIRE(state, sxml_schema_validator_memory_requirements(
                                 &configuration, &requirements) == SXML_STATUS_OK);
    alignas(max_align_t) uint8_t memory[2048];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(memory));
    sxml_schema_validator *validator = NULL;
    SXML_TEST_EXPECT(state, sxml_schema_validator_initialize(
                                (sxml_memory_view){.bytes = memory, .byte_count = 1},
                                &configuration, &validator) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_REQUIRE(
        state, sxml_schema_validator_initialize(
                   (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
                   &configuration, &validator) == SXML_STATUS_OK);
    sxml_validation_outcome outcome;
    /* No tree yet. */
    SXML_TEST_EXPECT(state, sxml_schema_validator_get_outcome(validator, &outcome) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_schema_validator_finalize(validator);
    sxml_schema_validator_finalize(NULL);
    sxml_schema_validator_destroy(NULL);
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

static void test_purchase_order(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_schema *schema = compile(processor, order_schema);
    SXML_TEST_REQUIRE(state, schema != NULL);
    expect_validity(state, (test_schema){.processor = processor, .schema = schema},
                    (expectation){.document = order_document,
                                  .validity = SXML_VALIDITY_VALID,
                                  .first_code = NULL});
    /* A child out of order (3.4.4.2 clause 2.4). */
    expect_validity(state, (test_schema){.processor = processor, .schema = schema},
                    (expectation){.document = "<purchaseOrder><comment>x</comment></purchaseOrder>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-complex-type.2.4"});
    /* Content that ends too early. */
    expect_validity(state, (test_schema){.processor = processor, .schema = schema},
                    (expectation){.document = "<purchaseOrder><shipTo><name>A</name></shipTo>"
                                              "</purchaseOrder>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-complex-type.2.4"});
    /* A fixed attribute value, a pattern, a bound, a required attribute,
       an undeclared attribute, character children in element-only
       content. */
    const char *const invalid_documents[][2] = {
        {"<items><item partNum='872-AA'><productName>x</productName><quantity>100</quantity>"
         "</item></items>",
         "cvc-maxExclusive-valid"},
        {"<items><item partNum='87-AA'><productName>x</productName><quantity>1</quantity>"
         "</item></items>",
         "cvc-pattern-valid"},
        {"<items><item><productName>x</productName><quantity>1</quantity></item></items>",
         "cvc-complex-type.4"},
        {"<items><item partNum='872-AA' other='1'><productName>x</productName>"
         "<quantity>1</quantity></item></items>",
         "cvc-complex-type.3.2.2"},
        {"<items>text</items>", "cvc-complex-type.2.3"},
        {"<unknown/>", "cvc-elt.1"},
    };
    sxml_schema_destroy(schema);
    schema = compile(processor,
                     "<xsd:schema xmlns:xsd='http://www.w3.org/2001/XMLSchema'>"
                     "<xsd:element name='items' type='Items'/>"
                     "<xsd:complexType name='Items'><xsd:sequence>"
                     "<xsd:element name='item' minOccurs='0' maxOccurs='unbounded'>"
                     "<xsd:complexType><xsd:sequence>"
                     "<xsd:element name='productName' type='xsd:string'/>"
                     "<xsd:element name='quantity'><xsd:simpleType>"
                     "<xsd:restriction base='xsd:positiveInteger'>"
                     "<xsd:maxExclusive value='100'/></xsd:restriction></xsd:simpleType>"
                     "</xsd:element></xsd:sequence>"
                     "<xsd:attribute name='partNum' use='required'><xsd:simpleType>"
                     "<xsd:restriction base='xsd:string'><xsd:pattern value='\\d{3}-[A-Z]{2}'/>"
                     "</xsd:restriction></xsd:simpleType></xsd:attribute>"
                     "</xsd:complexType></xsd:element></xsd:sequence></xsd:complexType>"
                     "</xsd:schema>");
    SXML_TEST_REQUIRE(state, schema != NULL);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(invalid_documents); case_index += 1) {
        expect_validity(state, (test_schema){.processor = processor, .schema = schema},
                        (expectation){.document = invalid_documents[case_index][0],
                                      .validity = SXML_VALIDITY_INVALID,
                                      .first_code = invalid_documents[case_index][1]});
    }
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* After a child the content model rejects, the rest of the content is
   matched on (glossary content_recovery): a missing element costs one
   report, a skipped extra one too, and later errors are still found. */
static void test_content_recovery(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_schema *schema = compile(processor, order_schema);
    SXML_TEST_REQUIRE(state, schema != NULL);
    const struct {
        const char *document;
        size_t report_count;
    } cases[] = {
        /* shipTo missing: billTo is where the model takes it next. */
        {"<purchaseOrder><billTo><name>R</name><zip>1</zip></billTo><items/></purchaseOrder>", 1},
        /* The same, with a later error in billTo's content. */
        {"<purchaseOrder><billTo><name>R</name><zip>x</zip></billTo><items/></purchaseOrder>", 2},
        /* An extra element is skipped; the content goes on after it. */
        {"<purchaseOrder><shipTo><name>A</name><zip>1</zip></shipTo><junk/>"
         "<billTo><name>R</name><zip>1</zip></billTo><items/></purchaseOrder>",
         1},
        /* items missing at the end: one report, not two. */
        {"<purchaseOrder><shipTo><name>A</name><zip>1</zip></shipTo><junk/></purchaseOrder>", 1},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        const validation_result result =
            validate(&(validation_request){.processor = processor,
                                           .schema = schema,
                                           .document = cases[case_index].document,
                                           .validation_mode = SXML_VALIDATION_MODE_STRICT});
        SXML_TEST_EXPECT(state, result.status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, result.outcome.validity == SXML_VALIDITY_INVALID);
        SXML_TEST_EXPECT(state, strcmp(result.log.first_code, "cvc-complex-type.2.4") == 0);
        SXML_TEST_EXPECT(state, result.log.report_count == cases[case_index].report_count);
    }
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

static void test_declarations_and_xsi(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_schema *schema = compile(processor, misc_schema);
    SXML_TEST_REQUIRE(state, schema != NULL);
    const expectation cases[] = {
        {.document = "<a>12</a>", .validity = SXML_VALIDITY_VALID, .first_code = NULL},
        {.document = "<a>5</a>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-minInclusive-valid"},
        {.document = "<a>x</a>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-datatype-valid.1.2.1"},
        {.document = "<a><b/></a>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-type.3.1.2"},
        {.document = "<a x='1'>12</a>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-type.3.1.1"},
        /* xsi:nil (3.3.4.1, clause 3). */
        {.document = "<n xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:nil='true'/>",
         .validity = SXML_VALIDITY_VALID,
         .first_code = NULL},
        {.document = "<n xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                     "xsi:nil='true'>1</n>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-elt.3.2.1"},
        {.document = "<a xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:nil='true'/>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-elt.3.1"},
        /* Default and fixed values (cvc-elt.5). */
        {.document = "<d/>", .validity = SXML_VALIDITY_VALID, .first_code = NULL},
        {.document = "<f>1.50</f>", .validity = SXML_VALIDITY_VALID, .first_code = NULL},
        {.document = "<f/>", .validity = SXML_VALIDITY_VALID, .first_code = NULL},
        {.document = "<f>2</f>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-elt.5.2.2.2.2"},
        /* xsi:type (cvc-elt.4). */
        {.document = "<b xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                     "xsi:type='Derived'><x/><y/></b>",
         .validity = SXML_VALIDITY_VALID,
         .first_code = NULL},
        {.document = "<b xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                     "xsi:type='Derived'><x/></b>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-complex-type.2.4"},
        {.document = "<b xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                     "xsi:type='Missing'><x/></b>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-elt.4.2"},
        {.document = "<b xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                     "xsi:type='xs:int' xmlns:xs='http://www.w3.org/2001/XMLSchema'>1</b>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-elt.4.3"},
        /* A skip wildcard, a default attribute. */
        {.document = "<e><any undeclared='1'><deep/></any></e>",
         .validity = SXML_VALIDITY_VALID,
         .first_code = NULL},
        {.document = "<e k='x'/>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-datatype-valid.1.2.1"},
        /* ID and IDREF (cvc-id). */
        {.document = "<ids><i id='a'/><i id='b' ref='a b'/></ids>",
         .validity = SXML_VALIDITY_VALID,
         .first_code = NULL},
        /* XSD 1.1, cvc-id.2: one value may identify one element twice. */
        {.document = "<ids><i id='a' id2='a'/></ids>",
         .validity = SXML_VALIDITY_VALID,
         .first_code = NULL},
        {.document = "<ids><i id='a'/><i id='a'/></ids>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-id.2"},
        {.document = "<ids><i ref='c'/></ids>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-id.1"},
        /* Empty content. */
        {.document = "<empty/>", .validity = SXML_VALIDITY_VALID, .first_code = NULL},
        {.document = "<empty>x</empty>",
         .validity = SXML_VALIDITY_INVALID,
         .first_code = "cvc-complex-type.2.1"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        expect_validity(state, (test_schema){.processor = processor, .schema = schema},
                        cases[case_index]);
    }
    /* Lax mode: an undeclared root is not known; a declared child inside
       it is still assessed. */
    validation_result result =
        validate(&(validation_request){.processor = processor,
                                       .schema = schema,
                                       .document = "<root><a>12</a></root>",
                                       .validation_mode = SXML_VALIDATION_MODE_LAX});
    SXML_TEST_EXPECT(state, result.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, result.outcome.validity == SXML_VALIDITY_NOT_KNOWN);
    result = validate(&(validation_request){.processor = processor,
                                            .schema = schema,
                                            .document = "<root><a>1</a></root>",
                                            .validation_mode = SXML_VALIDATION_MODE_LAX});
    SXML_TEST_EXPECT(state, result.outcome.validity == SXML_VALIDITY_INVALID);
    SXML_TEST_EXPECT(state, result.outcome.error_count == 1);
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* The document element of a document node. */
static sxml_node_reference node_of_child(sxml_item document) {
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_node_description description;
    if (sxml_item_get_node(document, &node) == SXML_STATUS_OK &&
        sxml_node_describe(node, &description) == SXML_STATUS_OK && description.has_children) {
        node.node_index = description.first_child;
    }
    return node;
}

/* The packet's test (phase-5.md, 5.7): fn:data of a validated <a>12</a>
   of a restriction of xs:integer is an item of that type. */
static void test_validated_node_has_typed_value(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_schema *schema = compile(processor, misc_schema);
    SXML_TEST_REQUIRE(state, schema != NULL);
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_processor_parse_document(
                                 processor,
                                 &(sxml_xml_parse_request){.input = TEXT("<a> 12 </a>"),
                                                           .source_name = TEXT("a.xml"),
                                                           .base_uri = TEXT("file:///a.xml")},
                                 &document) == SXML_STATUS_OK);
    sxml_item validated = {.representation = 0};
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    SXML_TEST_REQUIRE(
        state, sxml_schema_validate_node(schema,
                                         &(sxml_schema_validation_request){
                                             .node = document,
                                             .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                             .governing_type = {.index = SXML_TYPE_NONE},
                                             .report_sink = {.state = NULL, .operations = NULL},
                                             .error_record = &record},
                                         &validated) == SXML_STATUS_OK);
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    SXML_TEST_REQUIRE(state, sxml_item_get_node(validated, &node) == SXML_STATUS_OK);
    sxml_node_description description;
    SXML_TEST_REQUIRE(state, sxml_node_describe(node, &description) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, description.has_children);
    node.node_index = description.first_child;
    SXML_TEST_REQUIRE(state, sxml_node_describe(node, &description) == SXML_STATUS_OK);
    sxml_type_identifier big = {.index = 0};
    sxml_name_identifier big_name = {.index = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_name_pool_intern_name(sxml_processor_get_name_pool(processor),
                                                 &(sxml_name_request){.namespace_uri = {.index = 0},
                                                                      .local_name = TEXT("Big")},
                                                 &big_name) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_schema_find_type(schema, big_name, &big));
    SXML_TEST_EXPECT(state, description.type_annotation.index == big.index);
    sxml_sequence typed_value = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_node_make_typed_value(
                                 sxml_processor_get_item_heap(processor),
                                 &(sxml_node_value_request){.node = node, .error_record = NULL},
                                 &typed_value) == SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_sequence_get_item(typed_value, (sxml_item_request){.item_index = 0},
                                             &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_item_get_type_annotation(item).index == big.index);
    sxml_atomic_datum datum;
    SXML_TEST_REQUIRE(state, sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_INTEGER &&
                                datum.datum_details.integer == 12);
    sxml_sequence_release(typed_value);
    sxml_item_release(validated);
    /* The element alone: a parentless copy that keeps its base URI. */
    const sxml_item element = sxml_node_item_make(node_of_child(document));
    SXML_TEST_REQUIRE(
        state, sxml_schema_validate_node(
                   schema,
                   &(sxml_schema_validation_request){.node = element,
                                                     .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                                     .governing_type = {.index = SXML_TYPE_NONE}},
                   &validated) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_item_get_node(validated, &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_node_describe(node, &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.node_kind == SXML_NODE_KIND_ELEMENT &&
                                !description.has_parent &&
                                description.type_annotation.index == big.index);
    sxml_sequence base_uri = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_node_make_base_uri(
                                 sxml_processor_get_item_heap(processor),
                                 &(sxml_node_value_request){.node = node, .error_record = NULL},
                                 &base_uri) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_sequence_get_item(base_uri, (sxml_item_request){.item_index = 0},
                                                    &item) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(datum.datum_details.string.utf8, TEXT("file:///a.xml")));
    sxml_sequence_release(base_uri);
    sxml_item_release(validated);
    sxml_item_release(element);
    /* An invalid node: the first error's constraint name, no copy. */
    sxml_item invalid = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_processor_parse_document(
                                 processor,
                                 &(sxml_xml_parse_request){.input = TEXT("<a>5</a>"),
                                                           .source_name = TEXT("b.xml")},
                                 &invalid) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, sxml_schema_validate_node(schema,
                                         &(sxml_schema_validation_request){
                                             .node = invalid,
                                             .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                             .governing_type = {.index = SXML_TYPE_NONE},
                                             .report_sink = {.state = NULL, .operations = NULL},
                                             .error_record = &record},
                                         &validated) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(record.code.local_name, TEXT("cvc-minInclusive-valid")));
    sxml_item_release(invalid);
    sxml_item_release(document);
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* A default attribute and an element default enter the validated copy;
   whitespace in element-only content does not. */
static void test_validated_copy_has_defaults(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_schema *schema =
        compile(processor, "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                           "<xs:element name='r'><xs:complexType><xs:sequence>"
                           "<xs:element name='d' type='xs:int' default='7'/>"
                           "</xs:sequence><xs:attribute name='k' type='xs:token' "
                           "default='three'/></xs:complexType></xs:element></xs:schema>");
    SXML_TEST_REQUIRE(state, schema != NULL);
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_processor_parse_document(
                                 processor,
                                 &(sxml_xml_parse_request){.input = TEXT("<r>\n  <d/>\n</r>"),
                                                           .source_name = TEXT("r.xml")},
                                 &document) == SXML_STATUS_OK);
    sxml_item validated = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, sxml_schema_validate_node(
                   schema,
                   &(sxml_schema_validation_request){.node = document,
                                                     .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                                     .governing_type = {.index = SXML_TYPE_NONE}},
                   &validated) == SXML_STATUS_OK);
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    SXML_TEST_REQUIRE(state, sxml_item_get_node(validated, &node) == SXML_STATUS_OK);
    sxml_node_description description;
    SXML_TEST_REQUIRE(state, sxml_node_describe(node, &description) == SXML_STATUS_OK);
    node.node_index = description.first_child;
    SXML_TEST_REQUIRE(state, sxml_node_describe(node, &description) == SXML_STATUS_OK);
    /* r: one attribute (the default), one child (d), no text. */
    SXML_TEST_EXPECT(state, description.attribute_count == 1);
    SXML_TEST_EXPECT(state, description.has_children &&
                                description.first_child.index == description.last_child.index);
    sxml_node_reference attribute = node;
    attribute.node_index.index = node.node_index.index + 1U;
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_node_get_content(attribute, &content) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(content, TEXT("three")));
    sxml_node_reference child = node;
    child.node_index = description.first_child;
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_node_make_string_value(sxml_processor_get_item_heap(processor),
                                                         child, &text) == SXML_STATUS_OK);
    sxml_atomic_datum datum;
    SXML_TEST_REQUIRE(state, sxml_atomic_item_get_datum(text, &datum) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(datum.datum_details.string.utf8, TEXT("7")));
    sxml_item_release(text);
    /* r has element-only content: no typed value (FOTY0012). */
    sxml_sequence typed_value = sxml_sequence_empty();
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    SXML_TEST_EXPECT(state, sxml_node_make_typed_value(
                                sxml_processor_get_item_heap(processor),
                                &(sxml_node_value_request){.node = node, .error_record = &record},
                                &typed_value) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(record.code.local_name, TEXT("FOTY0012")));
    sxml_item_release(validated);
    sxml_item_release(document);
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* A schema with an assertion, a key and a type alternative, compiled and
   used twice (while parsing and by sxml_schema_validate_node) with
   allocation failing_number failing: every step either succeeds or fails
   with SXML_STATUS_OUT_OF_MEMORY, and nothing leaks (D23). */
static const char failure_schema[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
    "<xs:element name='list'><xs:complexType><xs:sequence>"
    "<xs:element name='item' maxOccurs='unbounded' type='Item'>"
    "<xs:alternative test='@kind = \"big\"' type='BigItem'/></xs:element>"
    "</xs:sequence><xs:assert test='count(item) le 3'/></xs:complexType>"
    "<xs:key name='ids'><xs:selector xpath='item'/><xs:field xpath='@id'/></xs:key>"
    "</xs:element>"
    "<xs:complexType name='Item'><xs:simpleContent><xs:extension base='xs:decimal'>"
    "<xs:attribute name='id' type='xs:ID' use='required'/>"
    "<xs:attribute name='kind' type='xs:token'/></xs:extension></xs:simpleContent>"
    "</xs:complexType>"
    "<xs:complexType name='BigItem'><xs:simpleContent><xs:restriction base='Item'>"
    "<xs:minInclusive value='100'/></xs:restriction></xs:simpleContent></xs:complexType>"
    "</xs:schema>";

static const char failure_document[] =
    "<list><item id='a'>1.5</item><item id='b' kind='big'>200</item></list>";

static sxml_status validate_with_failure(sxml_test_state *state, uint64_t failing_number) {
    sxml_counting_allocator *counting_allocator = NULL;
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number = failing_number},
        &counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    sxml_processor *processor = NULL;
    sxml_schema *schema = NULL;
    sxml_schema_validator *validator = NULL;
    sxml_xml_parser *parser = NULL;
    sxml_item document = {.representation = 0};
    sxml_item validated = {.representation = 0};
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
    status = sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                                   &processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_create(
            allocator,
            &(sxml_schema_configuration){
                .processor = processor,
                .schema_documents = {.schema_documents =
                                         &(sxml_schema_document){
                                             .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                             .document_text = TEXT(failure_schema),
                                             .document_uri = TEXT("file:///failure.xsd")},
                                     .schema_document_count = 1}},
            &schema);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_create(
            allocator,
            &(sxml_schema_validator_configuration){.schema = schema,
                                                   .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                                   .governing_type = {.index = SXML_TYPE_NONE}},
            &validator);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            allocator,
            &(sxml_xml_parser_configuration){.allocator = allocator,
                                             .name_pool = sxml_processor_get_name_pool(processor)},
            &parser);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            parser,
            &(sxml_xml_parse_request){.input = TEXT(failure_document),
                                      .source_name = TEXT("failure.xml")},
            sxml_schema_validator_as_tree_builder(validator));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_get_outcome(validator, &outcome);
    }
    if (status == SXML_STATUS_OK) {
        SXML_TEST_EXPECT(state, outcome.validity == SXML_VALIDITY_VALID);
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){.input = TEXT(failure_document),
                                      .source_name = TEXT("failure.xml")},
            &document);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validate_node(
            schema,
            &(sxml_schema_validation_request){.node = document,
                                              .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                              .governing_type = {.index = SXML_TYPE_NONE}},
            &validated);
    }
    sxml_item_release(validated);
    sxml_item_release(document);
    sxml_xml_parser_destroy(parser);
    sxml_schema_validator_destroy(validator);
    sxml_schema_destroy(schema);
    if (processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(processor);
        sxml_item_heap_clear_raised_error(item_heap);
        SXML_TEST_EXPECT(state, sxml_item_heap_read_counts(item_heap).live_cell_count == 0);
    }
    sxml_processor_destroy(processor);
    SXML_TEST_EXPECT(state,
                     sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count ==
                         0);
    sxml_counting_allocator_destroy(counting_allocator);
    return status;
}

static void test_validation_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 200000; failing_number += 1) {
        status = validate_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case schema_validate_test_cases[] = {
    {.name = "lifecycle_checks_its_arguments", .function = test_lifecycle_checks_its_arguments},
    {.name = "purchase_order", .function = test_purchase_order},
    {.name = "content_recovery", .function = test_content_recovery},
    {.name = "declarations_and_xsi", .function = test_declarations_and_xsi},
    {.name = "validated_node_has_typed_value", .function = test_validated_node_has_typed_value},
    {.name = "validated_copy_has_defaults", .function = test_validated_copy_has_defaults},
    {.name = "validation_survives_every_allocation_failure",
     .function = test_validation_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema_validate",
            .cases = schema_validate_test_cases,
            .case_count = SXML_TEST_COUNT(schema_validate_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
