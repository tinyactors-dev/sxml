/*
 * Unit tests for assertions and type alternatives (src/schema_assertion.c;
 * XSD 1.1 Part 1, 3.12 and 3.13; Part 2, 4.3.13; D175): the compile stage
 * (XPath Valid, as-props-correct.2 and ta-props-correct.2), assertion
 * facets with $value and no context item, xs:assert on the element's
 * validated subtree with $value of simple content, a dynamic error making
 * a test false, type alternatives on the element with its attributes and
 * the attributes it inherits, xpathDefaultNamespace, and comments being
 * excluded from what assertions see.
 *
 * Golden file: tests/schema_validate_test.c (a schema compiled from text,
 * documents validated while parsing with a report sink).
 */
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* src/schema_internal.h, example 3, in no namespace, with more cases. */
static const char message_schema[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
    "<xs:simpleType name='Percent'><xs:restriction base='xs:decimal'>"
    "<xs:minInclusive value='0'/><xs:maxInclusive value='100'/>"
    "<xs:assertion test='$value mod 5 = 0'/></xs:restriction></xs:simpleType>"
    "<xs:simpleType name='Even'><xs:restriction base='Percent'>"
    "<xs:assertion test='$value mod 2 = 0'/></xs:restriction></xs:simpleType>"
    "<xs:complexType name='Message'><xs:sequence>"
    "<xs:element name='subject' type='xs:string'/>"
    "<xs:element name='line' type='xs:string' maxOccurs='1000'/></xs:sequence>"
    "<xs:attribute name='kind' type='xs:token' use='required'/>"
    "<xs:attribute name='discount' type='Percent'/>"
    "<xs:assert test=\"@kind ne 'bulk' or count(line) le 2\"/></xs:complexType>"
    "<xs:complexType name='Urgent'><xs:complexContent><xs:extension base='Message'>"
    "<xs:sequence><xs:element name='deadline' type='xs:date'/></xs:sequence>"
    "</xs:extension></xs:complexContent></xs:complexType>"
    "<xs:element name='message' type='Message'>"
    "<xs:alternative test=\"@kind = 'urgent'\" type='Urgent'/>"
    "<xs:alternative type='Message'/></xs:element>"
    "<xs:element name='even' type='Even'/>"
    /* Simple content: $value is the typed value; '.' the element. */
    "<xs:element name='range'><xs:complexType><xs:simpleContent>"
    "<xs:extension base='xs:integer'><xs:attribute name='max' type='xs:integer'/>"
    "<xs:assert test='$value le @max'/></xs:extension></xs:simpleContent>"
    "</xs:complexType></xs:element>"
    /* A test that raises a dynamic error is false. */
    "<xs:element name='strict'><xs:complexType><xs:attribute name='n' type='xs:string'/>"
    "<xs:assert test='xs:integer(@n) gt 0'/></xs:complexType></xs:element>"
    /* Assertions do not see comments (3.13.4.1 clause 1.2). */
    "<xs:element name='quiet'><xs:complexType mixed='true'>"
    "<xs:assert test='empty(comment())'/></xs:complexType></xs:element>"
    /* Inherited attributes select alternatives (3.3.5.6, 3.12.4). */
    "<xs:element name='doc'><xs:complexType><xs:sequence>"
    "<xs:element ref='chapter' maxOccurs='unbounded'/></xs:sequence>"
    "<xs:attribute name='lang' type='xs:language' inheritable='true'/>"
    "</xs:complexType></xs:element>"
    "<xs:element name='chapter'>"
    "<xs:alternative test=\"@lang = 'de'\"><xs:complexType><xs:sequence>"
    "<xs:element name='de'/></xs:sequence>"
    "<xs:attribute name='lang' type='xs:language'/></xs:complexType></xs:alternative>"
    "<xs:alternative type='xs:error'/></xs:element>"
    "</xs:schema>";

/* xpathDefaultNamespace: unprefixed names in tests are of the target
   namespace (3.13.6.2 clause 2.2.3). */
static const char namespaced_schema[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns='urn:example'"
    " targetNamespace='urn:example' elementFormDefault='qualified'"
    " xpathDefaultNamespace='##targetNamespace'>"
    "<xs:element name='list'><xs:complexType><xs:sequence>"
    "<xs:element name='item' type='xs:integer' maxOccurs='unbounded'/></xs:sequence>"
    "<xs:assert test='count(item) = sum(item)'/></xs:complexType></xs:element>"
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

/* What compiling a schema gave. */
typedef struct compilation_result {
    sxml_status status;
    sxml_schema *schema;
    sxml_error_record error_record;
} compilation_result;

static compilation_result compile(sxml_processor *processor, const char *text) {
    compilation_result result = {.status = SXML_STATUS_OK, .schema = NULL};
    memset(&result.error_record, 0, sizeof(result.error_record));
    result.status = sxml_schema_create(
        sxml_default_allocator(),
        &(sxml_schema_configuration){
            .processor = processor,
            .schema_documents = {.schema_documents =
                                     &(sxml_schema_document){
                                         .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                         .document_text = sxml_byte_view_from_c_string(text),
                                         .document_uri = TEXT("file:///test.xsd")},
                                 .schema_document_count = 1},
            .error_record = &result.error_record},
        &result.schema);
    return result;
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

/* The processor and schema a test validates with. */
typedef struct test_schema {
    sxml_processor *processor;
    const sxml_schema *schema;
} test_schema;

/* A document to validate strictly, and what it should give. */
typedef struct expectation {
    const char *document;
    sxml_validity validity;
    /* The first report's constraint name, when invalid; NULL to not
       check it. */
    const char *first_code;
} expectation;

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
        status = sxml_xml_parser_create(
            allocator,
            &(sxml_xml_parser_configuration){
                .allocator = allocator, .name_pool = sxml_processor_get_name_pool(tested.processor)},
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
    sxml_xml_parser_destroy(parser);
    sxml_schema_validator_destroy(validator);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, outcome.validity == expected.validity);
    if (expected.validity == SXML_VALIDITY_VALID) {
        SXML_TEST_EXPECT(state, log.report_count == 0);
    } else if (expected.first_code != NULL) {
        SXML_TEST_EXPECT(state, strcmp(log.first_code, expected.first_code) == 0);
    }
}

static void test_assertion_facets(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    compilation_result compiled = compile(processor, message_schema);
    SXML_TEST_REQUIRE(state, compiled.status == SXML_STATUS_OK);
    const test_schema tested = {.processor = processor, .schema = compiled.schema};
    expect_validity(state, tested,
                    (expectation){.document = "<message kind='a' discount='15'><subject/><line/>"
                                              "</message>",
                                  .validity = SXML_VALIDITY_VALID});
    /* 12 passes the facets of the registry, then fails the assertion. */
    expect_validity(state, tested,
                    (expectation){.document = "<message kind='a' discount='12'><subject/><line/>"
                                              "</message>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-assertions-valid"});
    /* A restriction has its base's assertions, then its own (Part 2,
       4.3.13.2). */
    expect_validity(state, tested,
                    (expectation){.document = "<even>20</even>", .validity = SXML_VALIDITY_VALID});
    expect_validity(state, tested,
                    (expectation){.document = "<even>15</even>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-assertions-valid"});
    expect_validity(state, tested,
                    (expectation){.document = "<even>22</even>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-assertions-valid"});
    sxml_schema_destroy(compiled.schema);
    sxml_processor_destroy(processor);
}

static void test_assertions_of_complex_types(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    compilation_result compiled = compile(processor, message_schema);
    SXML_TEST_REQUIRE(state, compiled.status == SXML_STATUS_OK);
    const test_schema tested = {.processor = processor, .schema = compiled.schema};
    expect_validity(state, tested,
                    (expectation){.document = "<message kind='bulk'><subject/><line/><line/>"
                                              "</message>",
                                  .validity = SXML_VALIDITY_VALID});
    expect_validity(state, tested,
                    (expectation){.document = "<message kind='bulk'><subject/><line/><line/>"
                                              "<line/></message>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-assertion"});
    expect_validity(state, tested,
                    (expectation){.document = "<range max='5'>4</range>",
                                  .validity = SXML_VALIDITY_VALID});
    /* 10 le 9 compares integers ($value), not strings. */
    expect_validity(state, tested,
                    (expectation){.document = "<range max='9'>10</range>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-assertion"});
    expect_validity(state, tested,
                    (expectation){.document = "<strict n='3'/>", .validity = SXML_VALIDITY_VALID});
    /* FORG0001 makes the test false (3.13.4.1). */
    expect_validity(state, tested,
                    (expectation){.document = "<strict n='three'/>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-assertion"});
    expect_validity(state, tested,
                    (expectation){.document = "<quiet>a<!-- c -->b<?p i?></quiet>",
                                  .validity = SXML_VALIDITY_VALID});
    sxml_schema_destroy(compiled.schema);
    compiled = compile(processor, namespaced_schema);
    SXML_TEST_REQUIRE(state, compiled.status == SXML_STATUS_OK);
    const test_schema namespaced = {.processor = processor, .schema = compiled.schema};
    expect_validity(state, namespaced,
                    (expectation){.document = "<list xmlns='urn:example'><item>1</item>"
                                              "<item>1</item></list>",
                                  .validity = SXML_VALIDITY_VALID});
    expect_validity(state, namespaced,
                    (expectation){.document = "<list xmlns='urn:example'><item>1</item>"
                                              "<item>2</item></list>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-assertion"});
    sxml_schema_destroy(compiled.schema);
    sxml_processor_destroy(processor);
}

static void test_type_alternatives(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    compilation_result compiled = compile(processor, message_schema);
    SXML_TEST_REQUIRE(state, compiled.status == SXML_STATUS_OK);
    const test_schema tested = {.processor = processor, .schema = compiled.schema};
    /* kind='urgent' selects Urgent, which needs a deadline. */
    expect_validity(state, tested,
                    (expectation){.document = "<message kind='urgent'><subject/><line/>"
                                              "<deadline>2026-10-18</deadline></message>",
                                  .validity = SXML_VALIDITY_VALID});
    expect_validity(state, tested,
                    (expectation){.document = "<message kind='urgent'><subject/><line/>"
                                              "</message>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-complex-type.2.4"});
    /* The default alternative: Message, which has no deadline. */
    expect_validity(state, tested,
                    (expectation){.document = "<message kind='plain'><subject/><line/>"
                                              "<deadline>2026-10-18</deadline></message>",
                                  .validity = SXML_VALIDITY_INVALID,
                                  .first_code = "cvc-complex-type.2.4"});
    /* Selected by an inherited attribute, or by the element's own one. */
    expect_validity(state, tested,
                    (expectation){.document = "<doc lang='de'><chapter><de/></chapter>"
                                              "<chapter lang='de'><de/></chapter></doc>",
                                  .validity = SXML_VALIDITY_VALID});
    /* Without lang the default alternative xs:error is selected. */
    expect_validity(state, tested,
                    (expectation){.document = "<doc><chapter><de/></chapter></doc>",
                                  .validity = SXML_VALIDITY_INVALID});
    /* The element's own attribute wins over the inherited one. */
    expect_validity(state, tested,
                    (expectation){.document = "<doc lang='de'><chapter lang='fr'><de/></chapter>"
                                              "</doc>",
                                  .validity = SXML_VALIDITY_INVALID});
    sxml_schema_destroy(compiled.schema);
    sxml_processor_destroy(processor);
}

static void test_tests_must_be_valid_xpath(sxml_test_state *state) {
    sxml_processor *processor = open_processor();
    SXML_TEST_REQUIRE(state, processor != NULL);
    compilation_result compiled =
        compile(processor, "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                           "<xs:simpleType name='T'><xs:restriction base='xs:int'>"
                           "<xs:assertion test='$value ='/></xs:restriction></xs:simpleType>"
                           "</xs:schema>");
    SXML_TEST_EXPECT(state, compiled.status == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(compiled.error_record.code.local_name,
                                                  TEXT("as-props-correct.2")));
    sxml_schema_destroy(compiled.schema);
    /* Type alternatives declare no $value (3.13.6.2 clause 2.2.6). */
    compiled = compile(processor, "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                                  "<xs:element name='e'><xs:alternative test='$value'"
                                  " type='xs:int'/></xs:element></xs:schema>");
    SXML_TEST_EXPECT(state, compiled.status == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(compiled.error_record.code.local_name,
                                                  TEXT("ta-props-correct.2")));
    sxml_schema_destroy(compiled.schema);
    /* An unbound prefix is XPST0081, so the schema is invalid. */
    compiled = compile(processor, "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                                  "<xs:complexType name='C'><xs:assert test='p:x'/>"
                                  "</xs:complexType></xs:schema>");
    SXML_TEST_EXPECT(state, compiled.status == SXML_STATUS_STATIC_ERROR);
    sxml_schema_destroy(compiled.schema);
    sxml_processor_destroy(processor);
}

static const sxml_test_case schema_assertion_test_cases[] = {
    {.name = "assertion_facets", .function = test_assertion_facets},
    {.name = "assertions_of_complex_types", .function = test_assertions_of_complex_types},
    {.name = "type_alternatives", .function = test_type_alternatives},
    {.name = "tests_must_be_valid_xpath", .function = test_tests_must_be_valid_xpath},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema_assertion",
            .cases = schema_assertion_test_cases,
            .case_count = SXML_TEST_COUNT(schema_assertion_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
