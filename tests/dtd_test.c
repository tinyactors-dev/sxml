/*
 * Unit tests for DTD validation (src/dtd.c, src/schema_from_dtd.c, the
 * validation steps of src/xml.c; include/sxml/schema.h "DTD validation",
 * include/sxml/xml.h; packet 7.14b, D199): the validator's lifecycle,
 * valid documents with every kind of declaration, the constraints the
 * schema checks and those the parser checks, namespaced DTDs, a report
 * sink that receives every error, and the tree a validating parse builds.
 *
 * Golden file: tests/schema_validate_test.c (a processor, the report
 * sink, the error record).
 */
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

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

/* A processor and a parser that interns into its name pool. */
typedef struct dtd_fixture {
    sxml_processor *processor;
    sxml_xml_parser *parser;
} dtd_fixture;

static bool fixture_open(dtd_fixture *fixture) {
    const sxml_allocator allocator = sxml_default_allocator();
    *fixture = (dtd_fixture){.processor = NULL, .parser = NULL};
    if (sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                              &fixture->processor) != SXML_STATUS_OK) {
        return false;
    }
    return sxml_xml_parser_create(
               allocator,
               &(sxml_xml_parser_configuration){
                   .allocator = allocator,
                   .name_pool = sxml_processor_get_name_pool(fixture->processor),
                   .limits = {.maximum_depth = 0, .maximum_entity_expansion_byte_count = 0}},
               &fixture->parser) == SXML_STATUS_OK;
}

static void fixture_close(dtd_fixture *fixture) {
    sxml_xml_parser_destroy(fixture->parser);
    sxml_processor_destroy(fixture->processor);
}

/* What validating a document gave. */
typedef struct validation_result {
    sxml_status status;
    sxml_validity validity;
    /* The first error's code, from the error record (null sink) or the
       report log (sink). */
    char first_code[64];
    size_t report_count;
} validation_result;

/* A document to validate, with or without a report sink. */
typedef struct validation_request {
    const char *document;
    bool uses_sink;
} validation_request;

static validation_result validate(dtd_fixture *fixture, validation_request request) {
    validation_result result = {.status = SXML_STATUS_OUT_OF_MEMORY,
                                .validity = SXML_VALIDITY_INVALID,
                                .first_code = "",
                                .report_count = 0};
    report_log log = {.report_count = 0, .first_code = ""};
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof error_record);
    sxml_dtd_validator *validator = NULL;
    if (sxml_dtd_validator_create(
            sxml_default_allocator(),
            &(sxml_dtd_validator_configuration){
                .processor = fixture->processor,
                .report_sink =
                    request.uses_sink
                        ? (sxml_schema_report_sink){.state = &log, .operations = &log_operations}
                        : (sxml_schema_report_sink){.state = NULL, .operations = NULL},
                .error_record = &error_record},
            &validator) != SXML_STATUS_OK) {
        return result;
    }
    result.status = sxml_xml_parser_check(
        fixture->parser,
        &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(request.document),
                                  .source_name = TEXT("test.xml"),
                                  .validator = sxml_dtd_validator_as_xml_validator(validator)});
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
    if (result.status == SXML_STATUS_OK &&
        sxml_dtd_validator_get_outcome(validator, &outcome) == SXML_STATUS_OK) {
        result.validity = outcome.validity;
    }
    const sxml_byte_view code = error_record.code.local_name;
    const size_t byte_count = code.byte_count < sizeof(result.first_code) - 1
                                  ? code.byte_count
                                  : sizeof(result.first_code) - 1;
    if (byte_count != 0) {
        memcpy(result.first_code, code.bytes, byte_count);
    }
    result.first_code[byte_count] = '\0';
    result.report_count = log.report_count;
    if (request.uses_sink && log.report_count != 0) {
        memcpy(result.first_code, log.first_code, sizeof result.first_code);
    }
    sxml_dtd_validator_destroy(validator);
    sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(fixture->processor));
    return result;
}

/* Expects the document to be valid. */
static void expect_valid(sxml_test_state *state, dtd_fixture *fixture, const char *document) {
    const validation_result result =
        validate(fixture, (validation_request){.document = document, .uses_sink = false});
    const bool passed = result.status == SXML_STATUS_OK && result.validity == SXML_VALIDITY_VALID;
    if (!passed) {
        printf("    expected valid: %s\n    got %s, %s\n", document,
               (const char *)sxml_status_name(result.status).bytes, result.first_code);
    }
    SXML_TEST_EXPECT(state, passed);
}

/* An invalid document and the start of its first error's code. */
typedef struct invalid_document {
    const char *document;
    const char *code;
} invalid_document;

/* Expects the document to be invalid with the first error's code. */
static void expect_invalid(sxml_test_state *state, dtd_fixture *fixture,
                           invalid_document expected) {
    const char *document = expected.document;
    const char *code = expected.code;
    const validation_result result =
        validate(fixture, (validation_request){.document = document, .uses_sink = false});
    const bool passed = result.status == SXML_STATUS_DYNAMIC_ERROR &&
                        strncmp(result.first_code, code, strlen(code)) == 0;
    if (!passed) {
        printf("    expected invalid (%s): %s\n    got %s, %s\n", code, document,
               (const char *)sxml_status_name(result.status).bytes, result.first_code);
    }
    SXML_TEST_EXPECT(state, passed);
}

/* ---------------------------------------------------------------- Cases */

static void test_validator_lifecycle(sxml_test_state *state) {
    dtd_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_dtd_validator_memory_requirements(
                                &(sxml_dtd_validator_configuration){.processor = NULL},
                                &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_dtd_validator_configuration configuration = {.processor = fixture.processor};
    SXML_TEST_REQUIRE(state, sxml_dtd_validator_memory_requirements(
                                 &configuration, &requirements) == SXML_STATUS_OK);
    _Alignas(16) unsigned char memory[512];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof memory);
    sxml_dtd_validator *validator = NULL;
    SXML_TEST_EXPECT(state, sxml_dtd_validator_initialize(
                                (sxml_memory_view){.bytes = memory, .byte_count = 1},
                                &configuration, &validator) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_REQUIRE(state, sxml_dtd_validator_initialize(
                                 (sxml_memory_view){.bytes = memory, .byte_count = sizeof memory},
                                 &configuration, &validator) == SXML_STATUS_OK);
    /* Nothing parsed yet: no outcome. */
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_VALID, .error_count = 0};
    SXML_TEST_EXPECT(state, sxml_dtd_validator_get_outcome(validator, &outcome) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_dtd_validator_finalize(validator);
    sxml_dtd_validator_finalize(NULL);
    sxml_dtd_validator_destroy(NULL);
    fixture_close(&fixture);
}

static void test_valid_documents(sxml_test_state *state) {
    dtd_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* Every kind of content specification and attribute type (XML 1.0,
       3.2, 3.3). */
    expect_valid(state, &fixture,
                 "<!DOCTYPE r [\n"
                 "<!ELEMENT r (h, (p | l)*, f?)>\n"
                 "<!ELEMENT h EMPTY><!ELEMENT f ANY>\n"
                 "<!ELEMENT p (#PCDATA | e)*><!ELEMENT e (#PCDATA)>\n"
                 "<!ELEMENT l (e+)>\n"
                 "<!NOTATION gif SYSTEM 'image/gif'>\n"
                 "<!ENTITY pic SYSTEM 'pic.gif' NDATA gif>\n"
                 "<!ATTLIST r id ID #REQUIRED ref IDREF #IMPLIED refs IDREFS #IMPLIED\n"
                 "  kind (a|b) 'a' version CDATA #FIXED '1' tok NMTOKENS #IMPLIED\n"
                 "  img ENTITY #IMPLIED type NOTATION (gif) #IMPLIED xml:lang CDATA #IMPLIED>\n"
                 "]>\n"
                 "<r id='x' ref='x' refs='x x' tok='a b' img='pic' type='gif' xml:lang='en'>"
                 "<h/><p>text <e>e</e></p><l><e/><e/></l><f><h/>any</f></r>");
    /* A namespaced DTD: names resolve through the xmlns defaults and the
       document element's declarations. */
    expect_valid(state, &fixture,
                 "<!DOCTYPE p:a [<!ELEMENT p:a (p:b, q:c)><!ELEMENT p:b EMPTY>"
                 "<!ELEMENT q:c EMPTY><!ATTLIST p:a xmlns:p CDATA #FIXED 'urn:p' "
                 "xmlns:q CDATA #IMPLIED p:x CDATA #IMPLIED y CDATA #IMPLIED>]>"
                 "<p:a xmlns:q='urn:q' p:x='1' y='2'><p:b/><q:c/></p:a>");
    /* An element type in the xml namespace (Namespaces in XML 1.0, 3). */
    expect_valid(state, &fixture, "<!DOCTYPE xml:a [<!ELEMENT xml:a EMPTY>]><xml:a/>");
    /* White space in element content is allowed as character data. */
    expect_valid(state, &fixture,
                 "<!DOCTYPE r [<!ELEMENT r (e)*><!ELEMENT e EMPTY>]><r>\n <e/>\n</r>");
    fixture_close(&fixture);
}

static void test_schema_checked_constraints(sxml_test_state *state) {
    dtd_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* VC: Element Valid (3). */
    expect_invalid(state, &fixture,
                   (invalid_document){
                       .document =
                           "<!DOCTYPE r [<!ELEMENT r (a, b)><!ELEMENT a EMPTY><!ELEMENT b EMPTY>]>"
                           "<r><b/><a/></r>",
                       .code = "cvc-"});
    expect_invalid(state, &fixture,
                   (invalid_document){.document = "<!DOCTYPE r [<!ELEMENT r ANY>]><r><u/></r>",
                                      .code = "cvc-"});
    /* VC: Required Attribute, Fixed Attribute Default, Enumeration
       (3.3.2, 3.3.1), and an undeclared attribute. */
    expect_invalid(
        state, &fixture,
        (invalid_document){
            .document = "<!DOCTYPE r [<!ELEMENT r EMPTY><!ATTLIST r a CDATA #REQUIRED>]><r/>",
            .code = "cvc-"});
    expect_invalid(
        state, &fixture,
        (invalid_document){
            .document =
                "<!DOCTYPE r [<!ELEMENT r EMPTY><!ATTLIST r a CDATA #FIXED 'x'>]><r a='y'/>",
            .code = "cvc-"});
    expect_invalid(
        state, &fixture,
        (invalid_document){
            .document = "<!DOCTYPE r [<!ELEMENT r EMPTY><!ATTLIST r a (x|y) #IMPLIED>]><r a='z'/>",
            .code = "cvc-"});
    expect_invalid(state, &fixture,
                   (invalid_document){.document = "<!DOCTYPE r [<!ELEMENT r EMPTY>]><r a='1'/>",
                                      .code = "cvc-"});
    /* VC: ID, IDREF (3.3.1). */
    expect_invalid(
        state, &fixture,
        (invalid_document){.document = "<!DOCTYPE r [<!ELEMENT r (e*)><!ELEMENT e EMPTY>"
                                       "<!ATTLIST e id ID #IMPLIED>]><r><e id='a'/><e id='a'/></r>",
                           .code = "cvc-"});
    expect_invalid(
        state, &fixture,
        (invalid_document){.document =
                               "<!DOCTYPE r [<!ELEMENT r EMPTY><!ATTLIST r ref IDREF #IMPLIED>]>"
                               "<r ref='nowhere'/>",
                           .code = "cvc-"});
    fixture_close(&fixture);
}

static void test_parser_checked_constraints(sxml_test_state *state) {
    dtd_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* VC: Root Element Type (2.8) and no DTD at all. */
    expect_invalid(state, &fixture,
                   (invalid_document){.document = "<!DOCTYPE a [<!ELEMENT b EMPTY>]><b/>",
                                      .code = "xml-validity"});
    expect_invalid(state, &fixture, (invalid_document){.document = "<a/>", .code = "xml-validity"});
    /* VC: Unique Element Type Declaration, ID Attribute Default, One ID
       per Element Type, Notation Declared (3.2, 3.3.1, 4.2.2). */
    expect_invalid(
        state, &fixture,
        (invalid_document){.document = "<!DOCTYPE a [<!ELEMENT a EMPTY><!ELEMENT a ANY>]><a/>",
                           .code = "xml-validity"});
    expect_invalid(
        state, &fixture,
        (invalid_document){.document = "<!DOCTYPE a [<!ELEMENT a EMPTY><!ATTLIST a i ID 'x'>]><a/>",
                           .code = "xml-validity"});
    expect_invalid(
        state, &fixture,
        (invalid_document){
            .document =
                "<!DOCTYPE a [<!ELEMENT a EMPTY><!ATTLIST a i ID #IMPLIED j ID #IMPLIED>]><a/>",
            .code = "xml-validity"});
    expect_invalid(
        state, &fixture,
        (invalid_document){
            .document = "<!DOCTYPE a [<!ELEMENT a EMPTY><!ENTITY u SYSTEM 'u' NDATA none>]><a/>",
            .code = "xml-validity"});
    /* VC: Element Valid: EMPTY holds not even a comment (erratum E15). */
    expect_invalid(
        state, &fixture,
        (invalid_document){.document = "<!DOCTYPE a [<!ELEMENT a EMPTY>]><a><!--c--></a>",
                           .code = "xml-validity"});
    /* VC: Entity Declared (4.1), once a parameter entity is referenced. */
    expect_invalid(state, &fixture,
                   (invalid_document){
                       .document = "<!DOCTYPE a [<!ENTITY % p ''>%p;<!ELEMENT a ANY>]><a>&u;</a>",
                       .code = "xml-validity"});
    fixture_close(&fixture);
}

static void test_reports_go_to_the_sink(sxml_test_state *state) {
    dtd_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* With a sink the parse goes on: every error is reported and the
       outcome is invalid. */
    const validation_result result = validate(
        &fixture,
        (validation_request){.document = "<!DOCTYPE a [<!ELEMENT b EMPTY><!ELEMENT b ANY>]>"
                                         "<a x='1'/>",
                             .uses_sink = true});
    SXML_TEST_EXPECT(state, result.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, result.validity == SXML_VALIDITY_INVALID);
    SXML_TEST_EXPECT(state, result.report_count >= 2);
    SXML_TEST_EXPECT(state, strcmp(result.first_code, "xml-validity") == 0);
    fixture_close(&fixture);
}

static void test_the_tree_stays_untyped(sxml_test_state *state) {
    dtd_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_dtd_validator *validator = NULL;
    SXML_TEST_REQUIRE(state, sxml_dtd_validator_create(sxml_default_allocator(),
                                                       &(sxml_dtd_validator_configuration){
                                                           .processor = fixture.processor},
                                                       &validator) == SXML_STATUS_OK);
    /* The arena tree of a validating parse: is-id from the DTD, no type
       annotations (XDM 4.0, section 6). */
    sxml_item document_node = {.representation = 0};
    const sxml_status status = sxml_processor_parse_document(
        fixture.processor,
        &(sxml_xml_parse_request){
            .input = TEXT("<!DOCTYPE r [<!ELEMENT r EMPTY><!ATTLIST r i ID #REQUIRED "
                          "d CDATA 'v'>]><r i='k'/>"),
            .source_name = TEXT("test.xml"),
            .validator = sxml_dtd_validator_as_xml_validator(validator)},
        &document_node);
    SXML_TEST_REQUIRE(state, status == SXML_STATUS_OK);
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
    SXML_TEST_EXPECT(state, sxml_dtd_validator_get_outcome(validator, &outcome) == SXML_STATUS_OK &&
                                outcome.validity == SXML_VALIDITY_VALID);
    sxml_node_reference document;
    sxml_node_description description;
    SXML_TEST_REQUIRE(state, sxml_item_get_node(document_node, &document) == SXML_STATUS_OK &&
                                 sxml_node_describe(document, &description) == SXML_STATUS_OK);
    const sxml_node_reference root = {
        .tree = document.tree, .node_index = description.first_child, .namespace_number = 0};
    sxml_node_description root_description;
    SXML_TEST_REQUIRE(state, sxml_node_describe(root, &root_description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, root_description.type_annotation.index == SXML_TYPE_XS_UNTYPED);
    sxml_item_release(document_node);
    sxml_dtd_validator_destroy(validator);
    fixture_close(&fixture);
}

static const sxml_test_case dtd_test_cases[] = {
    {.name = "validator_lifecycle", .function = test_validator_lifecycle},
    {.name = "valid_documents", .function = test_valid_documents},
    {.name = "schema_checked_constraints", .function = test_schema_checked_constraints},
    {.name = "parser_checked_constraints", .function = test_parser_checked_constraints},
    {.name = "reports_go_to_the_sink", .function = test_reports_go_to_the_sink},
    {.name = "the_tree_stays_untyped", .function = test_the_tree_stays_untyped},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "dtd",
            .cases = dtd_test_cases,
            .case_count = SXML_TEST_COUNT(dtd_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
