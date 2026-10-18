/*
 * Unit tests for the compile stages READ and RESOLVE (src/schema_read.c,
 * src/schema_assemble.c, src/schema_resolve.c) and the text form
 * (src/schema_text.c): schema documents read into components, written out
 * after RESOLVE, and the representation constraints they report (XSD 1.1
 * Part 1, appendix B.3; D170).
 *
 * The three schemas are those of src/schema_internal.h; their text here is
 * what READ and RESOLVE write, so the properties of the later stages
 * (content types, registered types, primitive types, compiled XPath)
 * print as zero.
 *
 * Golden file: tests/xslt_read_test.c (documents read and their text form
 * compared; error codes of documents that break a rule).
 */
#include "processor_internal.h"
#include "schema_internal.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

/* The documents of one read and the further ones the resolver serves. */
typedef struct read_case {
    const sxml_schema_document *documents;
    size_t document_count;
    const sxml_resource_entry *resources;
    size_t resource_count;
} read_case;

/* What reading gave: the status, the first error, the errors counted. */
typedef struct read_outcome {
    sxml_status status;
    sxml_error_record error_record;
    size_t error_count;
    /* The text form after RESOLVE (NUL-terminated); empty on failure. */
    char text[16384];
} read_outcome;

static sxml_status count_report(void *state, const sxml_schema_report *report) {
    (void)report;
    size_t *error_count = state;
    *error_count += 1;
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations counting_sink = {.receive = count_report};

/* Reads and resolves a case with a counting report sink, so that every
   error is counted, and writes the text form. */
static void read_documents(const read_case *test_case, read_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    const sxml_allocator allocator = sxml_default_allocator();
    const sxml_resource_entry_view entries = {.resource_entries = test_case->resources,
                                              .resource_entry_count = test_case->resource_count};
    sxml_processor *processor = NULL;
    outcome->status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator, .resource_resolver = sxml_table_resource_resolver(&entries)},
        &processor);
    if (outcome->status != SXML_STATUS_OK) {
        return;
    }
    const sxml_schema_configuration configuration = {
        .processor = processor,
        .schema_documents = {.schema_documents = test_case->documents,
                             .schema_document_count = test_case->document_count},
        .report_sink = {.state = &outcome->error_count, .operations = &counting_sink},
        .error_record = &outcome->error_record};
    sxml_schema_components *components = NULL;
    outcome->status = sxml_schema_components_create(
        allocator, &(sxml_schema_components_configuration){.processor = processor}, &components);
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_schema_components_add_builtin_components(components);
    }
    sxml_schema_compilation compilation = {
        .configuration = &configuration, .components = components, .error_count = 0};
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_schema_read_documents(&compilation);
    }
    if (outcome->status == SXML_STATUS_OK && compilation.error_count == 0) {
        outcome->status = sxml_schema_resolve_references(&compilation);
    }
    if (outcome->status == SXML_STATUS_OK && compilation.error_count != 0) {
        outcome->status = SXML_STATUS_STATIC_ERROR;
    }
    if (outcome->status == SXML_STATUS_OK) {
        size_t byte_count = 0;
        if (sxml_schema_components_write_text(
                components,
                (sxml_memory_view){.bytes = (uint8_t *)outcome->text,
                                   .byte_count = sizeof(outcome->text) - 1U},
                &byte_count) != SXML_STATUS_OK) {
            byte_count = 0;
        }
        outcome->text[byte_count] = '\0';
    }
    /* The error record's views point into the components. */
    outcome->error_record.diagnostic.description = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    sxml_schema_components_destroy(components);
    sxml_processor_destroy(processor);
}

/* One schema document given as text. */
static sxml_schema_document text_document(const char *uri, const char *text) {
    return (sxml_schema_document){.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                  .document_text = sxml_byte_view_from_c_string(text),
                                  .document_uri = sxml_byte_view_from_c_string(uri)};
}

/* Reads one document and compares the text form with expected. */
static bool reads_as(const char *document, const char *expected) {
    const sxml_schema_document documents[] = {text_document("test.xsd", document)};
    static read_outcome outcome;
    read_documents(&(read_case){.documents = documents, .document_count = 1}, &outcome);
    const bool matches = outcome.status == SXML_STATUS_OK && strcmp(outcome.text, expected) == 0;
    if (!matches) {
        (void)fprintf(stderr, "status %d, %zu errors (%.*s), text:\n%s", (int)outcome.status,
                      outcome.error_count, (int)outcome.error_record.code.local_name.byte_count,
                      (const char *)outcome.error_record.code.local_name.bytes, outcome.text);
    }
    return matches;
}

/* Reads a case and compares the first error's constraint name. */
static bool fails_with(const read_case *test_case, const char *code) {
    static read_outcome outcome;
    read_documents(test_case, &outcome);
    const sxml_byte_view found = outcome.error_record.code.local_name;
    const bool matches = outcome.status == SXML_STATUS_STATIC_ERROR &&
                         sxml_byte_view_equals(found, sxml_byte_view_from_c_string(code));
    if (!matches) {
        (void)fprintf(stderr, "expected %s, got status %d code %.*s\n", code, (int)outcome.status,
                      (int)found.byte_count, (const char *)found.bytes);
    }
    return matches;
}

static bool document_fails_with(const char *document, const char *code) {
    const sxml_schema_document documents[] = {text_document("test.xsd", document)};
    return fails_with(&(read_case){.documents = documents, .document_count = 1}, code);
}

#define XS_SCHEMA "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'"

/* ------------------------------------------------------------ Examples */

/* Example 1 of src/schema_internal.h: a purchase order. */
static const char purchase_order[] =
    "<xsd:schema xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n"
    "  <xsd:element name=\"purchaseOrder\" type=\"PurchaseOrderType\">\n"
    "    <xsd:key name=\"partKey\">\n"
    "      <xsd:selector xpath=\"items/item\"/>\n"
    "      <xsd:field xpath=\"@partNum\"/>\n"
    "    </xsd:key>\n"
    "  </xsd:element>\n"
    "  <xsd:element name=\"comment\" type=\"xsd:string\"/>\n"
    "  <xsd:complexType name=\"PurchaseOrderType\">\n"
    "    <xsd:sequence>\n"
    "      <xsd:element name=\"shipTo\" type=\"USAddress\"/>\n"
    "      <xsd:element name=\"billTo\" type=\"USAddress\"/>\n"
    "      <xsd:element ref=\"comment\" minOccurs=\"0\"/>\n"
    "      <xsd:element name=\"items\" type=\"Items\"/>\n"
    "    </xsd:sequence>\n"
    "    <xsd:attribute name=\"orderDate\" type=\"xsd:date\"/>\n"
    "  </xsd:complexType>\n"
    "  <xsd:complexType name=\"USAddress\">\n"
    "    <xsd:sequence>\n"
    "      <xsd:element name=\"name\" type=\"xsd:string\"/>\n"
    "      <xsd:element name=\"street\" type=\"xsd:string\"/>\n"
    "      <xsd:element name=\"zip\" type=\"xsd:decimal\"/>\n"
    "    </xsd:sequence>\n"
    "    <xsd:attribute name=\"country\" type=\"xsd:NMTOKEN\" fixed=\"US\"/>\n"
    "  </xsd:complexType>\n"
    "  <xsd:complexType name=\"Items\">\n"
    "    <xsd:sequence>\n"
    "      <xsd:element name=\"item\" minOccurs=\"0\" maxOccurs=\"unbounded\">\n"
    "        <xsd:complexType>\n"
    "          <xsd:sequence>\n"
    "            <xsd:element name=\"productName\" type=\"xsd:string\"/>\n"
    "            <xsd:element name=\"quantity\">\n"
    "              <xsd:simpleType>\n"
    "                <xsd:restriction base=\"xsd:positiveInteger\">\n"
    "                  <xsd:maxExclusive value=\"100\"/>\n"
    "                </xsd:restriction>\n"
    "              </xsd:simpleType>\n"
    "            </xsd:element>\n"
    "            <xsd:element ref=\"comment\" minOccurs=\"0\"/>\n"
    "          </xsd:sequence>\n"
    "          <xsd:attribute name=\"partNum\" type=\"SKU\" use=\"required\"/>\n"
    "        </xsd:complexType>\n"
    "      </xsd:element>\n"
    "    </xsd:sequence>\n"
    "  </xsd:complexType>\n"
    "  <xsd:simpleType name=\"SKU\">\n"
    "    <xsd:restriction base=\"xsd:string\">\n"
    "      <xsd:pattern value=\"\\d{3}-[A-Z]{2}\"/>\n"
    "    </xsd:restriction>\n"
    "  </xsd:simpleType>\n"
    "</xsd:schema>\n";

static const char purchase_order_text[] =
    "document 1 \"test.xsd\" target-namespace none\n"
    "context 1 namespaces (xsd = http://www.w3.org/2001/XMLSchema) default-namespace none base-uri "
    "\"test.xsd\"\n"
    "type 56 PurchaseOrderType complex base 1 xsd:anyType restriction explicit-content particle 3 "
    "content empty attribute-uses (1) at 9:3\n"
    "type 57 USAddress complex base 1 xsd:anyType restriction explicit-content particle 8 content "
    "empty attribute-uses (2) at 18:3\n"
    "type 58 Items complex base 1 xsd:anyType restriction explicit-content particle 12 content "
    "empty at 26:3\n"
    "type 59 anonymous complex base 1 xsd:anyType restriction explicit-content particle 14 content "
    "empty attribute-uses (3) at 29:9\n"
    "type 60 anonymous atomic base 37 xsd:positiveInteger restriction facets (1) at 33:15\n"
    "type 61 SKU atomic base 6 xsd:string restriction facets (2) at 46:3\n"
    "element 1 purchaseOrder global type 56 PurchaseOrderType identity-constraints (1) at 2:3\n"
    "element 2 comment global type 6 xsd:string at 8:3\n"
    "element 3 shipTo local in type 56 type 57 USAddress at 11:7\n"
    "element 4 billTo local in type 56 type 57 USAddress at 12:7\n"
    "element 5 items local in type 56 type 58 Items at 14:7\n"
    "element 6 name local in type 57 type 6 xsd:string at 20:7\n"
    "element 7 street local in type 57 type 6 xsd:string at 21:7\n"
    "element 8 zip local in type 57 type 8 xsd:decimal at 22:7\n"
    "element 9 item local in type 58 type 59 at 28:7\n"
    "element 10 productName local in type 59 type 6 xsd:string at 31:13\n"
    "element 11 quantity local in type 59 type 60 at 32:13\n"
    "attribute 5 orderDate local in type 56 type 14 xsd:date at 16:5\n"
    "attribute 6 country local in type 57 type 41 xsd:NMTOKEN at 24:5\n"
    "attribute 7 partNum local in type 59 type 61 SKU at 41:11\n"
    "attribute-use 1 optional attribute 5 at 16:5\n"
    "attribute-use 2 optional attribute 6 fixed \"US\" context 1 at 24:5\n"
    "attribute-use 3 required attribute 7 at 41:11\n"
    "model-group 2 sequence (particle 4, particle 5, particle 6, particle 7) at 10:5\n"
    "model-group 3 sequence (particle 9, particle 10, particle 11) at 19:5\n"
    "model-group 4 sequence (particle 13) at 27:5\n"
    "model-group 5 sequence (particle 15, particle 16, particle 17) at 30:11\n"
    "particle 3 1..1 model-group 2 at 10:5\n"
    "particle 4 1..1 element 3 at 11:7\n"
    "particle 5 1..1 element 4 at 12:7\n"
    "particle 6 0..1 element 2 comment (reference 5) at 13:7\n"
    "particle 7 1..1 element 5 at 14:7\n"
    "particle 8 1..1 model-group 3 at 19:5\n"
    "particle 9 1..1 element 6 at 20:7\n"
    "particle 10 1..1 element 7 at 21:7\n"
    "particle 11 1..1 element 8 at 22:7\n"
    "particle 12 1..1 model-group 4 at 27:5\n"
    "particle 13 0..unbounded element 9 at 28:7\n"
    "particle 14 1..1 model-group 5 at 30:11\n"
    "particle 15 1..1 element 10 at 31:13\n"
    "particle 16 1..1 element 11 at 32:13\n"
    "particle 17 0..1 element 2 comment (reference 14) at 39:13\n"
    "identity-constraint 1 partKey key selector xpath 1 fields (xpath 2) at 3:5\n"
    "facet 1 max-exclusive \"100\" context 1 at 35:19\n"
    "facet 2 pattern \"\\d{3}-[A-Z]{2}\" context 1 at 48:7\n"
    "xpath 1 context 1 \"items/item\" at 4:7\n"
    "xpath 2 context 1 \"@partNum\" at 5:7\n"
    "reference 1 type PurchaseOrderType -> 56 at 2:3\n"
    "reference 2 type xsd:string -> 6 at 8:3\n"
    "reference 3 type USAddress -> 57 at 11:7\n"
    "reference 4 type USAddress -> 57 at 12:7\n"
    "reference 5 element comment -> 2 at 13:7\n"
    "reference 6 type Items -> 58 at 14:7\n"
    "reference 7 type xsd:date -> 14 at 16:5\n"
    "reference 8 type xsd:string -> 6 at 20:7\n"
    "reference 9 type xsd:string -> 6 at 21:7\n"
    "reference 10 type xsd:decimal -> 8 at 22:7\n"
    "reference 11 type xsd:NMTOKEN -> 41 at 24:5\n"
    "reference 12 type xsd:string -> 6 at 31:13\n"
    "reference 13 type xsd:positiveInteger -> 37 at 34:17\n"
    "reference 14 element comment -> 2 at 39:13\n"
    "reference 15 type SKU -> 61 at 41:11\n"
    "reference 16 type xsd:string -> 6 at 47:5\n";

static void test_purchase_order(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, reads_as(purchase_order, purchase_order_text));
}

/* Example 2: a recursive substitution group. */
static const char expression[] =
    "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"\n"
    "    xmlns:e=\"urn:example:expression\"\n"
    "    targetNamespace=\"urn:example:expression\"\n"
    "    elementFormDefault=\"qualified\">\n"
    "  <xs:complexType name=\"Expression\" abstract=\"true\"/>\n"
    "  <xs:element name=\"expression\" type=\"e:Expression\"\n"
    "      abstract=\"true\"/>\n"
    "  <xs:complexType name=\"Operation\">\n"
    "    <xs:complexContent>\n"
    "      <xs:extension base=\"e:Expression\">\n"
    "        <xs:sequence>\n"
    "          <xs:element ref=\"e:expression\" maxOccurs=\"unbounded\"/>\n"
    "        </xs:sequence>\n"
    "      </xs:extension>\n"
    "    </xs:complexContent>\n"
    "  </xs:complexType>\n"
    "  <xs:element name=\"number\" substitutionGroup=\"e:expression\">\n"
    "    <xs:complexType>\n"
    "      <xs:complexContent>\n"
    "        <xs:extension base=\"e:Expression\">\n"
    "          <xs:attribute name=\"value\" type=\"xs:decimal\"\n"
    "              use=\"required\"/>\n"
    "        </xs:extension>\n"
    "      </xs:complexContent>\n"
    "    </xs:complexType>\n"
    "  </xs:element>\n"
    "  <xs:element name=\"sum\" type=\"e:Operation\"\n"
    "      substitutionGroup=\"e:expression\"/>\n"
    "  <xs:element name=\"product\" type=\"e:Operation\"\n"
    "      substitutionGroup=\"e:expression\"/>\n"
    "  <xs:element name=\"difference\" type=\"e:Operation\"\n"
    "      substitutionGroup=\"e:sum\"/>\n"
    "</xs:schema>\n";

static const char expression_text[] =
    "document 1 \"test.xsd\" target-namespace urn:example:expression\n"
    "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema, e = urn:example:expression) "
    "default-namespace none base-uri \"test.xsd\"\n"
    "type 56 e:Expression complex abstract base 1 xs:anyType restriction content empty at 5:3\n"
    "type 57 e:Operation complex base 56 e:Expression extension explicit-content particle 3 "
    "content empty at 8:3\n"
    "type 58 anonymous complex base 56 e:Expression extension content empty attribute-uses (1) at "
    "18:5\n"
    "element 1 e:expression global abstract type 56 e:Expression members (2, 3, 4, 5) at 6:3\n"
    "element 2 e:number global type 58 substitution-group (1) at 17:3\n"
    "element 3 e:sum global type 57 e:Operation substitution-group (1) members (5) at 27:3\n"
    "element 4 e:product global type 57 e:Operation substitution-group (1) at 29:3\n"
    "element 5 e:difference global type 57 e:Operation substitution-group (3) at 31:3\n"
    "attribute 5 value local in type 58 type 8 xs:decimal at 21:11\n"
    "attribute-use 1 required attribute 5 at 21:11\n"
    "model-group 2 sequence (particle 4) at 11:9\n"
    "particle 3 1..1 model-group 2 at 11:9\n"
    "particle 4 1..unbounded element 1 e:expression (reference 3) at 12:11\n"
    "reference 1 type e:Expression -> 56 at 6:3\n"
    "reference 2 type e:Expression -> 56 at 10:7\n"
    "reference 3 element e:expression -> 1 at 12:11\n"
    "reference 4 element e:expression -> 1 at 17:3\n"
    "reference 5 type e:Expression -> 56 at 20:9\n"
    "reference 6 type xs:decimal -> 8 at 21:11\n"
    "reference 7 type e:Operation -> 57 at 27:3\n"
    "reference 8 element e:expression -> 1 at 27:3\n"
    "reference 9 type e:Operation -> 57 at 29:3\n"
    "reference 10 element e:expression -> 1 at 29:3\n"
    "reference 11 type e:Operation -> 57 at 31:3\n"
    "reference 12 element e:sum -> 3 at 31:3\n";

static void test_substitution_group(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, reads_as(expression, expression_text));
}

/* Example 3: XSD 1.1's assertion facet, assertions, open content, a large
   maxOccurs and type alternatives. */
static const char message[] =
    "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"\n"
    "    xmlns:m=\"urn:example:message\" targetNamespace=\"urn:example:message\"\n"
    "    elementFormDefault=\"qualified\"\n"
    "    xpathDefaultNamespace=\"##targetNamespace\">\n"
    "  <xs:simpleType name=\"Percent\">\n"
    "    <xs:restriction base=\"xs:decimal\">\n"
    "      <xs:minInclusive value=\"0\"/>\n"
    "      <xs:maxInclusive value=\"100\" fixed=\"true\"/>\n"
    "      <xs:assertion test=\"$value mod 5 = 0\"/>\n"
    "    </xs:restriction>\n"
    "  </xs:simpleType>\n"
    "  <xs:complexType name=\"Message\">\n"
    "    <xs:openContent mode=\"suffix\">\n"
    "      <xs:any namespace=\"##other\" processContents=\"skip\"/>\n"
    "    </xs:openContent>\n"
    "    <xs:sequence>\n"
    "      <xs:element name=\"subject\" type=\"xs:string\"/>\n"
    "      <xs:element name=\"line\" type=\"xs:string\" maxOccurs=\"1000\"/>\n"
    "    </xs:sequence>\n"
    "    <xs:attribute name=\"kind\" type=\"xs:token\" use=\"required\"/>\n"
    "    <xs:attribute name=\"discount\" type=\"m:Percent\"/>\n"
    "    <xs:assert test=\"@kind ne 'bulk' or count(line) le 10\"/>\n"
    "  </xs:complexType>\n"
    "  <xs:complexType name=\"Urgent\">\n"
    "    <xs:complexContent>\n"
    "      <xs:extension base=\"m:Message\">\n"
    "        <xs:sequence>\n"
    "          <xs:element name=\"deadline\" type=\"xs:dateTime\"/>\n"
    "        </xs:sequence>\n"
    "      </xs:extension>\n"
    "    </xs:complexContent>\n"
    "  </xs:complexType>\n"
    "  <xs:element name=\"message\" type=\"m:Message\">\n"
    "    <xs:alternative test=\"@kind = 'urgent'\" type=\"m:Urgent\"/>\n"
    "    <xs:alternative type=\"m:Message\"/>\n"
    "  </xs:element>\n"
    "</xs:schema>\n";

static const char message_text[] =
    "document 1 \"test.xsd\" target-namespace urn:example:message\n"
    "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema, m = urn:example:message) "
    "default-namespace urn:example:message base-uri \"test.xsd\"\n"
    "type 56 m:Percent atomic base 8 xs:decimal restriction facets (1, 2) assertions (1) at 5:3\n"
    "type 57 m:Message complex base 1 xs:anyType restriction explicit-content particle 3 content "
    "empty open-content suffix wildcard 3 attribute-uses (1, 2) assertions (2) at 12:3\n"
    "type 58 m:Urgent complex base 57 m:Message extension explicit-content particle 6 content "
    "empty at 24:3\n"
    "element 1 m:subject local in type 57 type 6 xs:string at 17:7\n"
    "element 2 m:line local in type 57 type 6 xs:string at 18:7\n"
    "element 3 m:deadline local in type 58 type 12 xs:dateTime at 28:11\n"
    "element 4 m:message global type 57 m:Message alternatives (1, 2) at 33:3\n"
    "attribute 5 kind local in type 57 type 39 xs:token at 20:5\n"
    "attribute 6 discount local in type 57 type 56 m:Percent at 21:5\n"
    "attribute-use 1 required attribute 5 at 20:5\n"
    "attribute-use 2 optional attribute 6 at 21:5\n"
    "model-group 2 sequence (particle 4, particle 5) at 16:5\n"
    "model-group 3 sequence (particle 7) at 27:9\n"
    "particle 3 1..1 model-group 2 at 16:5\n"
    "particle 4 1..1 element 1 at 17:7\n"
    "particle 5 1..1000 element 2 at 18:7\n"
    "particle 6 1..1 model-group 3 at 27:9\n"
    "particle 7 1..1 element 3 at 28:11\n"
    "wildcard 3 not (urn:example:message, absent) skip at 14:7\n"
    "alternative 1 test xpath 3 type 58 m:Urgent at 34:5\n"
    "alternative 2 type 57 m:Message at 35:5\n"
    "assertion 1 test xpath 1 at 9:7\n"
    "assertion 2 test xpath 2 at 22:5\n"
    "facet 1 min-inclusive \"0\" context 1 at 7:7\n"
    "facet 2 max-inclusive \"100\" fixed context 1 at 8:7\n"
    "xpath 1 context 1 \"$value mod 5 = 0\" at 9:7\n"
    "xpath 2 context 1 \"@kind ne 'bulk' or count(line) le 10\" at 22:5\n"
    "xpath 3 context 1 \"@kind = 'urgent'\" at 34:5\n"
    "reference 1 type xs:decimal -> 8 at 6:5\n"
    "reference 2 type xs:string -> 6 at 17:7\n"
    "reference 3 type xs:string -> 6 at 18:7\n"
    "reference 4 type xs:token -> 39 at 20:5\n"
    "reference 5 type m:Percent -> 56 at 21:5\n"
    "reference 6 type m:Message -> 57 at 26:7\n"
    "reference 7 type xs:dateTime -> 12 at 28:11\n"
    "reference 8 type m:Message -> 57 at 33:3\n"
    "reference 9 type m:Urgent -> 58 at 34:5\n"
    "reference 10 type m:Message -> 57 at 35:5\n";

static void test_xsd_11_components(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, reads_as(message, message_text));
}

/* ------------------------------------------------------ Representation */

/* What the schema for schema documents allows (Part 1, appendix A), with
   the constraint each role reports (D170). */
static void test_schema_for_schema_documents(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, document_fails_with("<schema/>", "schema_reference"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA ">", "schema_reference"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e' bogus='1'/>"
                                                          "</xs:schema>",
                                                "src-element"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element type='xs:string'/>"
                                                          "</xs:schema>",
                                                "src-element"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:complexType name='t'>"
                                                          "<xs:attribute name='a'/><xs:sequence/>"
                                                          "</xs:complexType></xs:schema>",
                                                "src-ct"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:simpleType name='s'/>"
                                                          "</xs:schema>",
                                                "src-simple-type"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e'>text"
                                                          "</xs:element></xs:schema>",
                                                "src-element"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e' "
                                                          "nillable='maybe'/></xs:schema>",
                                                "src-element"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:group name='g'><xs:sequence>"
                                                          "<xs:element name='a' maxOccurs='-1'/>"
                                                          "</xs:sequence></xs:group></xs:schema>",
                                                "src-element"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e' "
                                                          "block='list'/></xs:schema>",
                                                "src-element"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:notation name='n'/>"
                                                          "</xs:schema>",
                                                "n-props-correct"));
    SXML_TEST_EXPECT(state,
                     document_fails_with(XS_SCHEMA " targetNamespace=''/>", "sch-props-correct"));
    /* Annotations hold anything; other namespaces' attributes are
       allowed. */
    SXML_TEST_EXPECT(state, reads_as(XS_SCHEMA " xmlns:o='urn:o' o:x='1'><xs:annotation>"
                                               "<xs:appinfo><xs:element bogus='1'/></xs:appinfo>"
                                               "</xs:annotation></xs:schema>",

                                     "document 1 \"test.xsd\" target-namespace none\n"
                                     "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema, "
                                     "o = urn:o) default-namespace none base-uri \"test.xsd\"\n"));
}

/* The constraints of each element (src-element, src-attribute,
   src-simple-type, src-wildcard, src-identity-constraint, src-ta). */
static void test_representation_constraints(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e' default='a' "
                                                          "fixed='a'/></xs:schema>",
                                                "src-element"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:group name='g'><xs:sequence>"
                                                          "<xs:element name='a' ref='a'/>"
                                                          "</xs:sequence></xs:group></xs:schema>",
                                                "src-element.2.1"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='a'/><xs:group "
                                                          "name='g'><xs:sequence><xs:element "
                                                          "ref='a' nillable='true'/></xs:sequence>"
                                                          "</xs:group></xs:schema>",
                                                "src-element.2.2"));
    SXML_TEST_EXPECT(state,
                     document_fails_with(XS_SCHEMA "><xs:element name='e' "
                                                   "type='xs:string'><xs:simpleType>"
                                                   "<xs:restriction base='xs:string'/>"
                                                   "</xs:simpleType></xs:element></xs:schema>",
                                         "src-element.3"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:attributeGroup name='g'>"
                                                          "<xs:attribute name='a' use='required' "
                                                          "default='x'/></xs:attributeGroup>"
                                                          "</xs:schema>",
                                                "src-attribute.2"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:attribute name='xmlns'/>"
                                                          "</xs:schema>",
                                                "no-xmlns"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:simpleType name='s'>"
                                                          "<xs:restriction/></xs:simpleType>"
                                                          "</xs:schema>",
                                                "src-simple-type.2"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:simpleType name='s'><xs:list/>"
                                                          "</xs:simpleType></xs:schema>",
                                                "src-simple-type.3"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:simpleType name='s'><xs:union/>"
                                                          "</xs:simpleType></xs:schema>",
                                                "src-simple-type.4"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:complexType name='t'>"
                                                          "<xs:anyAttribute namespace='##any' "
                                                          "notNamespace='##local'/>"
                                                          "</xs:complexType></xs:schema>",
                                                "src-wildcard.1"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:complexType name='t'>"
                                                          "<xs:anyAttribute namespace='##other' "
                                                          "notQName='a'/></xs:complexType>"
                                                          "</xs:schema>",
                                                "w-props-correct.4"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e'><xs:key "
                                                          "name='k'><xs:selector xpath='a'/>"
                                                          "</xs:key></xs:element></xs:schema>",
                                                "src-identity-constraint"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e'>"
                                                          "<xs:alternative test='true()'/>"
                                                          "</xs:element></xs:schema>",
                                                "src-ta"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='a' id='x'/>"
                                                          "<xs:element name='b' id='x'/>"
                                                          "</xs:schema>",
                                                "cvc-id.2"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:complexType name='t'>"
                                                          "<xs:sequence><xs:element name='a' "
                                                          "targetNamespace='urn:other'/>"
                                                          "</xs:sequence></xs:complexType>"
                                                          "</xs:schema>",
                                                "src-element"));
}

/* Names: duplicates (sch-props-correct.2), references (src-resolve) and
   the namespaces a document may refer to (src-resolve.4.2). */
static void test_names_and_references(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e'/>"
                                                          "<xs:element name='e'/></xs:schema>",
                                                "sch-props-correct.2"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e' type='t'/>"
                                                          "</xs:schema>",
                                                "src-resolve"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e' "
                                                          "type='p:t'/></xs:schema>",
                                                "src-resolve"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA " xmlns:o='urn:o'><xs:element "
                                                          "name='e' type='o:t'/></xs:schema>",
                                                "src-resolve.4.2"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='e' "
                                                          "type='xs:untyped'/></xs:schema>",
                                                "src-resolve"));
}

/* Circular definitions RESOLVE finds. */
static void test_circular_definitions(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:group name='a'><xs:sequence>"
                                                          "<xs:group ref='b'/></xs:sequence>"
                                                          "</xs:group><xs:group name='b'>"
                                                          "<xs:choice><xs:group ref='a'/>"
                                                          "</xs:choice></xs:group></xs:schema>",
                                                "mg-props-correct.2"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:attributeGroup name='a'>"
                                                          "<xs:attributeGroup ref='b'/>"
                                                          "</xs:attributeGroup><xs:attributeGroup "
                                                          "name='b'><xs:attributeGroup ref='a'/>"
                                                          "</xs:attributeGroup></xs:schema>",
                                                "src-attribute_group.3"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:simpleType name='a'>"
                                                          "<xs:restriction base='b'/>"
                                                          "</xs:simpleType><xs:simpleType "
                                                          "name='b'><xs:list itemType='a'/>"
                                                          "</xs:simpleType></xs:schema>",
                                                "st-props-correct.2"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:complexType name='a'>"
                                                          "<xs:complexContent><xs:extension "
                                                          "base='a'/></xs:complexContent>"
                                                          "</xs:complexType></xs:schema>",
                                                "ct-props-correct.3"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA "><xs:element name='a' "
                                                          "substitutionGroup='b'/><xs:element "
                                                          "name='b' substitutionGroup='a'/>"
                                                          "</xs:schema>",
                                                "e-props-correct.6"));
}

/* Conditional inclusion (Part 1, 4.2.2): the elements an XSD 1.1
   processor ignores are not read, so their duplicate names are none. */
static void test_conditional_inclusion(sxml_test_state *state) {
    SXML_TEST_EXPECT(
        state, reads_as(XS_SCHEMA " xmlns:vc='http://www.w3.org/2007/XMLSchema-versioning'>"
                                  "<xs:element name='e' vc:minVersion='1.2'/>"
                                  "<xs:element name='e' vc:maxVersion='1.1'/>"
                                  "<xs:element name='e' vc:typeAvailable='xs:string xs:nothing'/>"
                                  "<xs:element name='e' vc:typeUnavailable='xs:string'/>"
                                  "<xs:element name='e' vc:facetUnavailable='xs:pattern'/>"
                                  "<xs:element name='e' vc:minVersion='1.0' vc:maxVersion='1.2'/>"
                                  "</xs:schema>",
                        "document 1 \"test.xsd\" target-namespace none\n"
                        "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema, vc = "
                        "http://www.w3.org/2007/XMLSchema-versioning) default-namespace none "
                        "base-uri \"test.xsd\"\n"
                        "element 1 e global type 1 xs:anyType at 1:365\n"));
    SXML_TEST_EXPECT(state,
                     document_fails_with(XS_SCHEMA " xmlns:vc='http://www.w3.org/2007/"
                                                   "XMLSchema-versioning'><xs:element name='e' "
                                                   "vc:minVersion='one'/></xs:schema>",
                                         "src-cip.1"));
}

/* ------------------------------------------------------------ Assembly */

#define INCLUDING                                                                                  \
    XS_SCHEMA " xmlns:t='urn:t' targetNamespace='urn:t'><xs:include "                              \
              "schemaLocation='part.xsd'/><xs:element name='e' type='t:p'/></xs:schema>"

static const char ch_text[] =
    "document 1 \"file:///s/main.xsd\" target-namespace urn:t\n"
    "document 2 \"file:///s/part.xsd\" target-namespace urn:t\n"
    "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema, t = urn:t) default-namespace "
    "none base-uri \"file:///s/main.xsd\"\n"
    "context 2 namespaces (xs = http://www.w3.org/2001/XMLSchema) default-namespace none base-uri "
    "\"file:///s/part.xsd\"\n"
    "type 56 t:p list base 57 t:q restriction at 2:1:56\n"
    "type 57 t:q list base 3 xs:anySimpleType list item 29 xs:int at 2:1:122\n"
    "element 1 t:e global type 56 t:p at 1:1:135\n"
    "reference 1 type t:p -> 56 at 1:1:135\n"
    "reference 2 type t:q -> 57 at 2:1:80\n"
    "reference 3 type xs:int -> 29 at 2:1:146\n";

/* A chameleon inclusion (Part 1, 4.2.3, F.1): the included document's
   components and unqualified references are in the includer's
   namespace. */
static void test_chameleon_inclusion(sxml_test_state *state) {
    const sxml_schema_document documents[] = {text_document("file:///s/main.xsd", INCLUDING)};
    const sxml_resource_entry resources[] = {
        {.uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///s/part.xsd"),
         .content = SXML_BYTE_VIEW_FROM_LITERAL(XS_SCHEMA "><xs:simpleType name='p'>"
                                                          "<xs:restriction base='q'/>"
                                                          "</xs:simpleType><xs:simpleType "
                                                          "name='q'><xs:list itemType="
                                                          "'xs:int'/></xs:simpleType>"
                                                          "</xs:schema>")}};
    static read_outcome outcome;
    read_documents(&(read_case){.documents = documents,
                                .document_count = 1,
                                .resources = resources,
                                .resource_count = 1},
                   &outcome);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(outcome.text, ch_text) == 0);
    if (strcmp(outcome.text, ch_text) != 0) {
        (void)fprintf(stderr, "%s", outcome.text);
    }
}

/* Documents that cannot be read or are of another namespace (4.2.3 to
   4.2.6). */
static void test_document_references(sxml_test_state *state) {
    const sxml_schema_document missing[] = {text_document("file:///s/main.xsd", INCLUDING)};
    SXML_TEST_EXPECT(state, fails_with(&(read_case){.documents = missing, .document_count = 1},
                                       "src-include.1"));
    SXML_TEST_EXPECT(state, reads_as(XS_SCHEMA "><xs:import namespace='urn:o' "
                                               "schemaLocation='o.xsd'/></xs:schema>",
                                     "document 1 \"test.xsd\" target-namespace none\n"
                                     "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema) "
                                     "default-namespace none base-uri \"test.xsd\"\n"));
    const sxml_resource_entry other[] = {
        {.uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///s/part.xsd"),
         .content = SXML_BYTE_VIEW_FROM_LITERAL(XS_SCHEMA " targetNamespace='urn:x'/>")}};
    SXML_TEST_EXPECT(state, fails_with(&(read_case){.documents = missing,
                                                    .document_count = 1,
                                                    .resources = other,
                                                    .resource_count = 1},
                                       "src-include.2.1"));
    const sxml_schema_document importing[] = {text_document(
        "file:///s/main.xsd", XS_SCHEMA "><xs:import namespace='urn:o' schemaLocation='part.xsd'/>"
                                        "</xs:schema>")};
    SXML_TEST_EXPECT(state, fails_with(&(read_case){.documents = importing,
                                                    .document_count = 1,
                                                    .resources = other,
                                                    .resource_count = 1},
                                       "src-import.3.1"));
    SXML_TEST_EXPECT(state, document_fails_with(XS_SCHEMA " targetNamespace='urn:t'><xs:import "
                                                          "namespace='urn:t'/></xs:schema>",
                                                "src-import.1.1"));
    SXML_TEST_EXPECT(state,
                     document_fails_with(XS_SCHEMA "><xs:import/></xs:schema>", "src-import.1.2"));
}

static const char rd_text[] = "document 1 \"file:///s/main.xsd\" target-namespace none\n"
                              "document 2 \"file:///s/base.xsd\" target-namespace none\n"
                              "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema) "
                              "default-namespace none base-uri \"file:///s/main.xsd\"\n"
                              "context 2 namespaces (xs = http://www.w3.org/2001/XMLSchema) "
                              "default-namespace none base-uri \"file:///s/base.xsd\"\n"
                              "context 3 namespaces (xs = http://www.w3.org/2001/XMLSchema) "
                              "default-namespace none base-uri \"file:///s/main.xsd\"\n"
                              "type 56 s atomic base 6 xs:string restriction at 2:1:56\n"
                              "type 57 s atomic base 56 s restriction facets (1) at 1:1:95\n"
                              "element 1 e global type 57 s at 2:1:130\n"
                              "facet 1 max-length \"3\" context 3 at 1:1:144\n"
                              "reference 1 type xs:string -> 6 at 2:1:80\n"
                              "reference 2 type s -> 57 at 2:1:130\n"
                              "reference 3 type s -> 56 at 1:1:119\n";

static const char ov_text[] = "document 1 \"file:///s/main.xsd\" target-namespace none\n"
                              "document 2 \"file:///s/base.xsd\" target-namespace none\n"
                              "context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema) "
                              "default-namespace none base-uri \"file:///s/main.xsd\"\n"
                              "context 2 namespaces (xs = http://www.w3.org/2001/XMLSchema) "
                              "default-namespace none base-uri \"file:///s/base.xsd\"\n"
                              "type 56 s atomic base 6 xs:string restriction at 2:1:56\n"
                              "element 1 e global type 56 s at 2:1:130\n"
                              "element 2 e global type 29 xs:int at 1:1:95\n"
                              "reference 1 type xs:string -> 6 at 2:1:80\n"
                              "reference 2 type s -> 56 at 2:1:130\n"
                              "reference 3 type xs:int -> 29 at 1:1:95\n";

/* xs:redefine (4.2.4): the redefinition's base is the original, and what
   it must be; xs:override (4.2.5) replaces what it names. */
static void test_redefine_and_override(sxml_test_state *state) {
    const sxml_resource_entry original[] = {
        {.uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///s/base.xsd"),
         .content = SXML_BYTE_VIEW_FROM_LITERAL(XS_SCHEMA "><xs:simpleType name='s'>"
                                                          "<xs:restriction base='xs:string'/>"
                                                          "</xs:simpleType><xs:element name='e' "
                                                          "type='s'/></xs:schema>")}};
    const sxml_schema_document redefining[] = {text_document(
        "file:///s/main.xsd", XS_SCHEMA "><xs:redefine schemaLocation='base.xsd'>"
                                        "<xs:simpleType name='s'><xs:restriction base='s'>"
                                        "<xs:maxLength value='3'/></xs:restriction></xs:simpleType>"
                                        "</xs:redefine></xs:schema>")};
    static read_outcome outcome;
    read_documents(&(read_case){.documents = redefining,
                                .document_count = 1,
                                .resources = original,
                                .resource_count = 1},
                   &outcome);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(outcome.text, rd_text) == 0);
    if (strcmp(outcome.text, rd_text) != 0) {
        (void)fprintf(stderr, "%s", outcome.text);
    }
    const sxml_schema_document not_derived[] = {text_document(
        "file:///s/main.xsd", XS_SCHEMA "><xs:redefine schemaLocation='base.xsd'>"
                                        "<xs:simpleType name='s'><xs:restriction base='xs:int'/>"
                                        "</xs:simpleType></xs:redefine></xs:schema>")};
    SXML_TEST_EXPECT(state, fails_with(&(read_case){.documents = not_derived,
                                                    .document_count = 1,
                                                    .resources = original,
                                                    .resource_count = 1},
                                       "src-redefine.5"));
    const sxml_schema_document unknown[] = {text_document(
        "file:///s/main.xsd", XS_SCHEMA "><xs:redefine schemaLocation='base.xsd'>"
                                        "<xs:simpleType name='u'><xs:restriction base='u'/>"
                                        "</xs:simpleType></xs:redefine></xs:schema>")};
    SXML_TEST_EXPECT(state, fails_with(&(read_case){.documents = unknown,
                                                    .document_count = 1,
                                                    .resources = original,
                                                    .resource_count = 1},
                                       "src-redefine.2"));
    const sxml_schema_document overriding[] = {text_document(
        "file:///s/main.xsd", XS_SCHEMA "><xs:override schemaLocation='base.xsd'>"
                                        "<xs:element name='e' type='xs:int'/><xs:element name='f'/>"
                                        "</xs:override></xs:schema>")};
    read_documents(&(read_case){.documents = overriding,
                                .document_count = 1,
                                .resources = original,
                                .resource_count = 1},
                   &outcome);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(outcome.text, ov_text) == 0);
    if (strcmp(outcome.text, ov_text) != 0) {
        (void)fprintf(stderr, "%s", outcome.text);
    }
    const sxml_schema_document twice[] = {text_document(
        "file:///s/main.xsd", XS_SCHEMA "><xs:override schemaLocation='base.xsd'>"
                                        "<xs:element name='e' type='xs:int'/><xs:element name='e'/>"
                                        "</xs:override></xs:schema>")};
    SXML_TEST_EXPECT(state, fails_with(&(read_case){.documents = twice,
                                                    .document_count = 1,
                                                    .resources = original,
                                                    .resource_count = 1},
                                       "src-override"));
}

/* With a report sink, every error is reported, not just the first. */
static void test_every_error_reported(sxml_test_state *state) {
    const sxml_schema_document documents[] = {text_document(
        "test.xsd", XS_SCHEMA "><xs:element name='a' type='x'/><xs:element name='b' type='y'/>"
                              "<xs:element name='a'/></xs:schema>")};
    static read_outcome outcome;
    read_documents(&(read_case){.documents = documents, .document_count = 1}, &outcome);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_STATIC_ERROR);
    /* READ's duplicate; RESOLVE does not run after it. */
    SXML_TEST_EXPECT(state, outcome.error_count == 1);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(outcome.error_record.code.local_name,
                                           SXML_BYTE_VIEW_FROM_LITERAL("sch-props-correct.2")));
    const sxml_schema_document unresolved[] = {text_document(
        "test.xsd", XS_SCHEMA "><xs:element name='a' type='x'/><xs:element name='b' type='y'/>"
                              "</xs:schema>")};
    read_documents(&(read_case){.documents = unresolved, .document_count = 1}, &outcome);
    SXML_TEST_EXPECT(state, outcome.error_count == 2);
}

static const sxml_test_case schema_read_test_cases[] = {
    {.name = "purchase_order", .function = test_purchase_order},
    {.name = "substitution_group", .function = test_substitution_group},
    {.name = "xsd_11_components", .function = test_xsd_11_components},
    {.name = "schema_for_schema_documents", .function = test_schema_for_schema_documents},
    {.name = "representation_constraints", .function = test_representation_constraints},
    {.name = "names_and_references", .function = test_names_and_references},
    {.name = "circular_definitions", .function = test_circular_definitions},
    {.name = "conditional_inclusion", .function = test_conditional_inclusion},
    {.name = "chameleon_inclusion", .function = test_chameleon_inclusion},
    {.name = "document_references", .function = test_document_references},
    {.name = "redefine_and_override", .function = test_redefine_and_override},
    {.name = "every_error_reported", .function = test_every_error_reported},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema_read",
            .cases = schema_read_test_cases,
            .case_count = SXML_TEST_COUNT(schema_read_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
