/*
 * Unit tests for identity constraints (src/schema_identity.c; XSD 1.1
 * Part 1, 3.11; D177): the selector and field subsets of XPath and
 * c-props-correct when a schema is compiled, and unique, key and keyref
 * when a document is validated (cvc-identity-constraint), with key
 * sequences compared by XSD's equality and node tables carried up from
 * descendants (3.11.5).
 *
 * Golden file: tests/schema_validate_test.c (schemas compiled from text,
 * validation while parsing with a report sink).
 */
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static sxml_processor *open_processor(void) {
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_processor *processor = NULL;
    if (sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                              &processor) != SXML_STATUS_OK) {
        return NULL;
    }
    return processor;
}

/* Every report a compilation or validation sends, the first one's code
   kept. */
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

/* Compiles text; *log receives the reports. NULL when it does not
   compile. */
typedef struct compilation_request {
    sxml_processor *processor;
    const char *text;
    report_log *log;
} compilation_request;

static sxml_schema *compile(compilation_request request) {
    sxml_schema *schema = NULL;
    *request.log = (report_log){.report_count = 0, .first_code = {0}};
    if (sxml_schema_create(
            sxml_default_allocator(),
            &(sxml_schema_configuration){
                .processor = request.processor,
                .schema_documents = {.schema_documents =
                                         &(sxml_schema_document){
                                             .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                             .document_text =
                                                 sxml_byte_view_from_c_string(request.text),
                                             .document_uri = TEXT("file:///test.xsd")},
                                     .schema_document_count = 1},
                .report_sink = {.state = request.log, .operations = &log_operations}},
            &schema) != SXML_STATUS_OK) {
        return NULL;
    }
    return schema;
}

/* A schema whose element r has one identity constraint, written in
   between its type and its end. */
static const char constraint_schema_start[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns:p='urn:p'>"
    "<xs:element name='r'><xs:complexType><xs:sequence>"
    "<xs:element name='e' minOccurs='0' maxOccurs='unbounded'><xs:complexType>"
    "<xs:attribute name='a' type='xs:string'/></xs:complexType></xs:element>"
    "</xs:sequence></xs:complexType>";
static const char constraint_schema_end[] = "</xs:element></xs:schema>";

/* Compiling a unique with selector and field gives first_code (NULL:
   compiles). */
typedef struct subset_case {
    const char *selector;
    const char *field;
    const char *first_code;
} subset_case;

static void expect_subset(sxml_test_state *state, sxml_processor *processor, subset_case tested) {
    char text[1024];
    const int written =
        snprintf(text, sizeof(text),
                 "%s<xs:unique name='u'><xs:selector xpath=\"%s\"/>"
                 "<xs:field xpath=\"%s\"/></xs:unique>%s",
                 constraint_schema_start, tested.selector, tested.field, constraint_schema_end);
    SXML_TEST_REQUIRE(state, written > 0 && (size_t)written < sizeof(text));
    report_log log;
    sxml_schema *schema =
        compile((compilation_request){.processor = processor, .text = text, .log = &log});
    if (tested.first_code == NULL) {
        SXML_TEST_EXPECT(state, schema != NULL);
        SXML_TEST_EXPECT(state, log.report_count == 0);
    } else {
        SXML_TEST_EXPECT(state, schema == NULL);
        SXML_TEST_EXPECT(state, strcmp(log.first_code, tested.first_code) == 0);
    }
    sxml_schema_destroy(schema);
}

static void test_selector_and_field_subsets(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    static const subset_case cases[] = {
        /* 3.11.6.2 and 3.11.6.3. */
        {.selector = "e", .field = "@a", .first_code = NULL},
        {.selector = ".//e", .field = ".", .first_code = NULL},
        {.selector = "./e | .//p:e | *", .field = "e/@a", .first_code = NULL},
        {.selector = "child::e/ child :: *", .field = "attribute::a", .first_code = NULL},
        {.selector = ".", .field = "p:*/@p:*", .first_code = NULL},
        {.selector = ".//.", .field = "./e/.", .first_code = NULL},
        {.selector = ".//e/e/e", .field = "@a", .first_code = NULL},
        {.selector = "e/.//e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "e/e/@a", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "@a/e/e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "self::*", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "e//e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "//e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "../e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "e[1]", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "@a", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "*:e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "Q{}e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "(e)", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "e | @a", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "descendant::e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "q:e", .field = "@a", .first_code = "c-selector-xpath"},
        {.selector = "e", .field = "", .first_code = "c-fields-xpath"},
        {.selector = "e", .field = "@a/e", .first_code = "c-fields-xpath"},
        {.selector = "e", .field = "string(@a)", .first_code = "c-fields-xpath"},
        {.selector = "e", .field = "self::*", .first_code = "c-fields-xpath"},
        {.selector = "e", .field = ".//@a", .first_code = NULL},
        {.selector = "e", .field = "e//@a", .first_code = "c-fields-xpath"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        expect_subset(state, processor, cases[case_index]);
    }
    sxml_processor_destroy(processor);
}

static void test_keyref_needs_a_matching_key(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    static const char *const texts[] = {
        "<xs:key name='k'><xs:selector xpath='e'/><xs:field xpath='@a'/></xs:key>"
        "<xs:keyref name='f' refer='k'><xs:selector xpath='e'/><xs:field "
        "xpath='@a'/>"
        "<xs:field xpath='@a'/></xs:keyref>",
        "<xs:keyref name='g' refer='f'><xs:selector xpath='e'/><xs:field "
        "xpath='@a'/>"
        "</xs:keyref>"
        "<xs:keyref name='f' refer='g'><xs:selector xpath='e'/><xs:field "
        "xpath='@a'/>"
        "</xs:keyref>",
    };
    static const char *const codes[] = {"c-props-correct.2", "c-props-correct.1"};
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(texts); text_index += 1) {
        char text[1024];
        const int written = snprintf(text, sizeof(text), "%s%s%s", constraint_schema_start,
                                     texts[text_index], constraint_schema_end);
        SXML_TEST_REQUIRE(state, written > 0 && (size_t)written < sizeof(text));
        report_log log;
        sxml_schema *schema =
            compile((compilation_request){.processor = processor, .text = text, .log = &log});
        SXML_TEST_EXPECT(state, schema == NULL);
        SXML_TEST_EXPECT(state, strcmp(log.first_code, codes[text_index]) == 0);
        sxml_schema_destroy(schema);
    }
    sxml_processor_destroy(processor);
}

/* --------------------------------------------------------- Validation */

/* The purchase order of src/schema_internal.h, example 1, shortened,
   with its key, a keyref and a unique. */
static const char order_schema[] =
    "<xsd:schema xmlns:xsd='http://www.w3.org/2001/XMLSchema'>"
    "<xsd:element name='purchaseOrder'><xsd:complexType><xsd:sequence>"
    "<xsd:element name='items'><xsd:complexType><xsd:sequence>"
    "<xsd:element name='item' minOccurs='0' "
    "maxOccurs='unbounded'><xsd:complexType>"
    "<xsd:sequence><xsd:element name='productName' type='xsd:string' "
    "minOccurs='0'/>"
    "<xsd:element name='quantity' type='xsd:decimal' minOccurs='0' "
    "nillable='true'/>"
    "</xsd:sequence>"
    "<xsd:attribute name='partNum' type='xsd:decimal'/>"
    "<xsd:attribute name='code' type='xsd:float'/>"
    "</xsd:complexType></xsd:element></xsd:sequence></xsd:complexType></"
    "xsd:element>"
    "<xsd:element name='order' minOccurs='0' "
    "maxOccurs='unbounded'><xsd:complexType>"
    "<xsd:attribute name='part' "
    "type='xsd:integer'/></xsd:complexType></xsd:element>"
    "</xsd:sequence></xsd:complexType>"
    "<xsd:key name='partKey'><xsd:selector xpath='items/item'/>"
    "<xsd:field xpath='@partNum'/></xsd:key>"
    "<xsd:keyref name='orderRef' refer='partKey'><xsd:selector xpath='order'/>"
    "<xsd:field xpath='@part'/></xsd:keyref>"
    "<xsd:unique name='nameUnique'><xsd:selector xpath='items/item'/>"
    "<xsd:field xpath='productName'/></xsd:unique>"
    "</xsd:element></xsd:schema>";

/* Validates document strictly; first_code NULL means valid. A
   report_count other than 0 is the number of reports expected. */
typedef struct expectation {
    const char *document;
    const char *first_code;
    size_t report_count;
} expectation;

typedef struct test_schema {
    sxml_processor *processor;
    const sxml_schema *schema;
} test_schema;

static void expect_validity(sxml_test_state *state, test_schema tested, expectation expected) {
    const sxml_allocator allocator = sxml_default_allocator();
    report_log log = {.report_count = 0, .first_code = {0}};
    sxml_schema_validator *validator = NULL;
    sxml_status status = sxml_schema_validator_create(
        allocator,
        &(sxml_schema_validator_configuration){
            .schema = tested.schema,
            .validation_mode = SXML_VALIDATION_MODE_STRICT,
            .governing_type = {.index = SXML_TYPE_NONE},
            .result_builder = {.state = NULL, .operations = NULL},
            .report_sink = {.state = &log, .operations = &log_operations},
            .error_record = NULL},
        &validator);
    sxml_xml_parser *parser = NULL;
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xml_parser_create(allocator,
                                   &(sxml_xml_parser_configuration){
                                       .allocator = allocator,
                                       .name_pool = sxml_processor_get_name_pool(tested.processor)},
                                   &parser);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            parser,
            &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(expected.document),
                                      .source_name = TEXT("test.xml")},
            sxml_schema_validator_as_tree_builder(validator));
    }
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_get_outcome(validator, &outcome);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (expected.first_code == NULL) {
        SXML_TEST_EXPECT(state, outcome.validity == SXML_VALIDITY_VALID);
        SXML_TEST_EXPECT(state, log.report_count == 0);
    } else {
        SXML_TEST_EXPECT(state, outcome.validity == SXML_VALIDITY_INVALID);
        SXML_TEST_EXPECT(state, strcmp(log.first_code, expected.first_code) == 0);
    }
    if (expected.report_count != 0) {
        SXML_TEST_EXPECT(state, log.report_count == expected.report_count);
    }
    sxml_xml_parser_destroy(parser);
    sxml_schema_validator_destroy(validator);
}

static void test_key_keyref_and_unique(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    report_log log;
    sxml_schema *schema =
        compile((compilation_request){.processor = processor, .text = order_schema, .log = &log});
    SXML_TEST_REQUIRE(state, schema != NULL);
    const test_schema tested = {.processor = processor, .schema = schema};
    static const expectation cases[] = {
        {.document = "<purchaseOrder><items><item "
                     "partNum='1'><productName>A</productName>"
                     "</item><item partNum='2'/></items><order part='2'/><order "
                     "part='1'/>"
                     "</purchaseOrder>",
         .first_code = NULL},
        /* 4.2.2: 1 and 1.0 are one xs:decimal. */
        {.document = "<purchaseOrder><items><item partNum='1'/><item "
                     "partNum='1.0'/></items>"
                     "</purchaseOrder>",
         .first_code = "cvc-identity-constraint.4.2.2"},
        /* 4.2.1: a key's field without a value. */
        {.document = "<purchaseOrder><items><item partNum='1'/><item/></items>"
                     "</purchaseOrder>",
         .first_code = "cvc-identity-constraint.4.2.1"},
        /* 4.3: the xs:integer 3 has no key; xs:integer 2 equals the
           xs:decimal 2.0. */
        {.document = "<purchaseOrder><items><item partNum='2.0'/></items><order "
                     "part='2'/>"
                     "<order part='3'/></purchaseOrder>",
         .first_code = "cvc-identity-constraint.4.3"},
        /* 4.1: a unique; members without the field take no part. */
        {.document = "<purchaseOrder><items><item "
                     "partNum='1'><productName>A</productName>"
                     "</item><item partNum='2'/><item partNum='3'/><item partNum='4'>"
                     "<productName>A</productName></item></items></purchaseOrder>",
         .first_code = "cvc-identity-constraint.4.1"},
        /* A keyref without a value is not checked. */
        {.document = "<purchaseOrder><items/><order/></purchaseOrder>", .first_code = NULL},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        expect_validity(state, tested, cases[case_index]);
    }
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* Fields: element values, lists, nillable keys, more than one node, the
   primitive types of XSD's equality. */
static const char field_schema[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'"
    " xmlns:t='urn:t' targetNamespace='urn:t' elementFormDefault='qualified'"
    " xpathDefaultNamespace='##targetNamespace'>"
    "<xs:simpleType name='Ints'><xs:list itemType='xs:int'/></xs:simpleType>"
    "<xs:element name='r'><xs:complexType><xs:sequence>"
    "<xs:element name='v' minOccurs='0' maxOccurs='unbounded'><xs:complexType>"
    "<xs:sequence><xs:element name='n' type='t:Ints' minOccurs='0' "
    "maxOccurs='2'"
    " nillable='true'/>"
    "<xs:element name='c' minOccurs='0'><xs:complexType><xs:sequence>"
    "<xs:element name='n' "
    "type='xs:int'/></xs:sequence></xs:complexType></xs:element>"
    "</xs:sequence><xs:attribute name='s' type='xs:string'/>"
    "<xs:attribute name='u' type='xs:anyURI'/></xs:complexType></xs:element>"
    "</xs:sequence></xs:complexType>"
    "<xs:unique name='byList'><xs:selector xpath='v'/><xs:field "
    "xpath='n'/></xs:unique>"
    "<xs:unique name='byText'><xs:selector xpath='v'/><xs:field "
    "xpath='@s|@u'/></xs:unique>"
    "<xs:unique name='byChild'><xs:selector xpath='v'/><xs:field "
    "xpath='c'/></xs:unique>"
    "</xs:element></xs:schema>";

static void test_fields_and_equality(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    report_log log;
    sxml_schema *schema =
        compile((compilation_request){.processor = processor, .text = field_schema, .log = &log});
    SXML_TEST_REQUIRE(state, schema != NULL);
    const test_schema tested = {.processor = processor, .schema = schema};
    static const expectation cases[] = {
        {.document = "<r xmlns='urn:t'><v><n>1 2</n></v><v><n>1</n></v><v><n>2 "
                     "1</n></v></r>",
         .first_code = NULL},
        {.document = "<r xmlns='urn:t'><v><n> 1  2 </n></v><v><n>1 2</n></v></r>",
         .first_code = "cvc-identity-constraint.4.1"},
        /* A nilled element has no value. */
        {.document = "<r xmlns='urn:t' "
                     "xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance'>"
                     "<v><n xsi:nil='true'/></v><v><n xsi:nil='true'/></v></r>",
         .first_code = NULL},
        /* Clause 3: two nodes. */
        {.document = "<r xmlns='urn:t'><v><n>1</n><n>2</n></v></r>",
         .first_code = "cvc-identity-constraint.3"},
        /* Clause 3: a node of complex content. */
        {.document = "<r xmlns='urn:t'><v><c><n>1</n></c></v></r>",
         .first_code = "cvc-identity-constraint.3"},
        /* xs:string and xs:anyURI are different primitive types. */
        {.document = "<r xmlns='urn:t'><v s='a'/><v u='a'/></r>", .first_code = NULL},
        {.document = "<r xmlns='urn:t'><v u='a'/><v u='a'/></r>",
         .first_code = "cvc-identity-constraint.4.1"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        expect_validity(state, tested, cases[case_index]);
    }
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* Keyrefs whose key is declared on descendants (3.11.5): the node
   tables of g elements carry up to r, and a key sequence two g elements
   give with different nodes leaves r's table. */
static const char nested_schema[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
    "<xs:element name='r'><xs:complexType><xs:sequence>"
    "<xs:element name='g' minOccurs='0' maxOccurs='unbounded'><xs:complexType>"
    "<xs:sequence><xs:element name='k' minOccurs='0' maxOccurs='unbounded'>"
    "<xs:complexType><xs:attribute name='id' "
    "type='xs:int'/></xs:complexType></xs:element>"
    "</xs:sequence></xs:complexType>"
    "<xs:key name='gk'><xs:selector xpath='k'/><xs:field xpath='@id'/></xs:key>"
    "</xs:element>"
    "<xs:element name='ref' minOccurs='0' "
    "maxOccurs='unbounded'><xs:complexType>"
    "<xs:attribute name='to' type='xs:int'/></xs:complexType></xs:element>"
    "</xs:sequence></xs:complexType>"
    "<xs:keyref name='toKey' refer='gk'><xs:selector xpath='ref'/><xs:field "
    "xpath='@to'/>"
    "</xs:keyref>"
    "</xs:element></xs:schema>";

static void test_node_tables_carry_up(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    report_log log;
    sxml_schema *schema =
        compile((compilation_request){.processor = processor, .text = nested_schema, .log = &log});
    SXML_TEST_REQUIRE(state, schema != NULL);
    const test_schema tested = {.processor = processor, .schema = schema};
    static const expectation cases[] = {
        {.document = "<r><g><k id='1'/><k id='2'/></g><g><k id='3'/></g>"
                     "<ref to='3'/><ref to='1'/></r>",
         .first_code = NULL},
        {.document = "<r><g><k id='1'/></g><ref to='2'/></r>",
         .first_code = "cvc-identity-constraint.4.3"},
        /* 1 is in both g elements' tables with different nodes. */
        {.document = "<r><g><k id='1'/></g><g><k id='1'/></g><ref to='1'/></r>",
         .first_code = "cvc-identity-constraint.4.3"},
        {.document = "<r><g><k id='1'/></g><g><k id='1'/></g><ref to='2'/></r>",
         .first_code = "cvc-identity-constraint.4.3"},
        /* The key itself is checked in each g. */
        {.document = "<r><g><k id='1'/><k id='1'/></g></r>",
         .first_code = "cvc-identity-constraint.4.2.2"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        expect_validity(state, tested, cases[case_index]);
    }
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

/* A field whose node is not valid for its type: the node is reported
   (cvc-datatype-valid.1.2.1) and has no [schema actual value] (Part 1,
   3.3.5.2 and 3.2.5), so its member is not qualified (3.11.4 clause 4):
   a key reports 4.2.1 as well, a unique and a keyref take no part; the
   validation ends, it does not fail. */
static void test_invalid_field_values(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    report_log log;
    sxml_schema *schema =
        compile((compilation_request){.processor = processor, .text = order_schema, .log = &log});
    SXML_TEST_REQUIRE(state, schema != NULL);
    sxml_schema *field_types =
        compile((compilation_request){.processor = processor, .text = field_schema, .log = &log});
    SXML_TEST_REQUIRE(state, field_types != NULL);
    static const expectation cases[] = {
        /* A key field: the attribute's error, then 4.2.1. */
        {.document = "<purchaseOrder><items><item partNum='1'/><item partNum='x 1'/>"
                     "</items></purchaseOrder>",
         .first_code = "cvc-datatype-valid.1.2.1",
         .report_count = 2},
        /* A keyref field: not checked. */
        {.document = "<purchaseOrder><items><item partNum='1'/></items><order part='a b'/>"
                     "</purchaseOrder>",
         .first_code = "cvc-datatype-valid.1.2.1",
         .report_count = 1},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        expect_validity(state, (test_schema){.processor = processor, .schema = schema},
                        cases[case_index]);
    }
    static const expectation field_cases[] = {
        /* A unique field of an element: two invalid values are not
           duplicates. */
        {.document = "<r xmlns='urn:t'><v><n>1 x</n></v><v><n>1 x</n></v></r>",
         .first_code = "cvc-datatype-valid.1.2.1",
         .report_count = 2},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(field_cases); case_index += 1) {
        expect_validity(state, (test_schema){.processor = processor, .schema = field_types},
                        field_cases[case_index]);
    }
    sxml_schema_destroy(field_types);
    sxml_schema_destroy(schema);
    sxml_processor_destroy(processor);
}

static const sxml_test_case schema_identity_test_cases[] = {
    {.name = "selector_and_field_subsets", .function = test_selector_and_field_subsets},
    {.name = "keyref_needs_a_matching_key", .function = test_keyref_needs_a_matching_key},
    {.name = "key_keyref_and_unique", .function = test_key_keyref_and_unique},
    {.name = "fields_and_equality", .function = test_fields_and_equality},
    {.name = "node_tables_carry_up", .function = test_node_tables_carry_up},
    {.name = "invalid_field_values", .function = test_invalid_field_values},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema_identity",
            .cases = schema_identity_test_cases,
            .case_count = SXML_TEST_COUNT(schema_identity_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
