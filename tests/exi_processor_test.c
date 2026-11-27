/*
 * Tests for EXI in the processor (src/function_library_exi.c,
 * src/serialize_exi.c, the options reader of src/exi.c; packet 8.13,
 * D333) through the public API: the options reader of an exi:header
 * element (each option and each refusal of EXI 1.0, Appendix C and
 * section 5.4), exi:serialize and exi:parse from XQuery, the output method
 * exi:exi from an output declaration and from the parameter reader,
 * fn:serialize refusing it, the functions' absence without the feature,
 * sxml_processor_parse_exi_document and the schema resolver's cache.
 * Every case checks for leaked cells and allocations.
 *
 * Golden file: tests/function_library_binary_test.c.
 */
#include "sxml/exi.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xquery.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The options document's namespace, as a default namespace declaration. */
#define HEADER_START "<header xmlns='http://www.w3.org/2009/exi'>"
#define HEADER_END "</header>"
/* The prolog binding exi, which a host binds as the sxml command does. */
#define PROLOG "declare namespace exi = 'urn:sxml:exi'; "

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

typedef struct fixture_options {
    bool names_feature;
    sxml_resource_resolver resource_resolver;
} fixture_options;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(const fixture_options *options, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number = 0},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    const sxml_optional_feature feature = sxml_exi_feature();
    status = sxml_processor_create(allocator,
                                   &(sxml_processor_configuration){
                                       .allocator = allocator,
                                       .resource_resolver = options->resource_resolver,
                                       .optional_features = {.optional_features = &feature,
                                                             .optional_feature_count = options->names_feature ? 1 : 0}},
                                   &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(), .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when nothing leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak &&
                      sxml_counting_allocator_read_counts(test_fixture->counting_allocator).live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

static sxml_byte_view text_of(const char *text) {
    return (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = strlen(text)};
}

/* Parses XML text into a document node of the fixture's processor. */
static sxml_status parse_text(const fixture *test_fixture, const char *xml_text, sxml_item *document) {
    return sxml_processor_parse_document(test_fixture->processor, &(sxml_xml_parse_request){.input = text_of(xml_text)},
                                         document);
}

/* The document element of a document node. */
static sxml_node_reference document_element_of(sxml_item document) {
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_node_description description;
    if (sxml_item_get_node(document, &node) != SXML_STATUS_OK ||
        sxml_node_describe(node, &description) != SXML_STATUS_OK || !description.has_children) {
        return node;
    }
    return (sxml_node_reference){.tree = node.tree, .node_index = description.first_child, .namespace_number = 0};
}

/* Compiles query_text as XQuery, evaluates it and serializes the result
   with the parameters its prolog declares, or, when uses_adaptive, by the
   adaptive method, into the fixture's output buffer (cleared first). */
typedef struct query_run {
    const char *query_text;
    bool uses_adaptive;
} query_run;

static sxml_status run_query(const fixture *test_fixture, query_run run, sxml_error_record *error_record) {
    memset(error_record, 0, sizeof(*error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xquery_query *query = NULL;
    sxml_status status =
        sxml_xquery_query_create(fixture_allocator(test_fixture),
                                 &(sxml_xquery_query_configuration){.processor = test_fixture->processor,
                                                                    .expression_text = text_of(run.query_text),
                                                                    .source_name = TEXT("test.xq"),
                                                                    .error_record = error_record},
                                 &query);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status =
        sxml_xquery_query_evaluate(query, &(sxml_xquery_evaluation_request){.error_record = error_record}, &result);
    sxml_serialization_parameters parameters = sxml_xquery_query_get_serialization_parameters(query);
    if (run.uses_adaptive) {
        parameters = (sxml_serialization_parameters){.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                                     .has_item_separator = true,
                                                     .item_separator = TEXT(" ")};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(sxml_processor_get_item_heap(test_fixture->processor),
                                         &(sxml_serialization_request){.value = result,
                                                                       .serialization_parameters = parameters,
                                                                       .error_record = error_record},
                                         sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    }
    sxml_sequence_release(result);
    sxml_xquery_query_destroy(query);
    return status;
}

/* Runs pair[0] and checks its adaptive output pair[1], or the error code
   it raises ("!CODE", the local name; "!exi:CODE" also checks
   urn:sxml:exi). */
static void expect_query(sxml_test_state *state, const fixture *test_fixture, const char *const pair[2]) {
    sxml_error_record error_record;
    const sxml_status status =
        run_query(test_fixture, (query_run){.query_text = pair[0], .uses_adaptive = true}, &error_record);
    const char *expected = pair[1];
    if (expected[0] != '!') {
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(
            state, sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer), text_of(expected)));
        return;
    }
    SXML_TEST_EXPECT(state, sxml_status_has_error_record(status));
    const char *code = expected + 1;
    if (strncmp(code, "exi:", 4) == 0) {
        code += 4;
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.namespace_uri, TEXT("urn:sxml:exi")));
    }
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name, text_of(code)));
}

static void expect_queries(sxml_test_state *state, const char *const (*cases)[2], size_t case_count) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true}, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        expect_query(state, &test_fixture, cases[case_index]);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------- Options reader */

/* Reads header_text's document element with a fresh options reader. */
typedef struct options_reading {
    const fixture *test_fixture;
    const char *header_text;
} options_reading;

static sxml_status read_options(options_reading reading, sxml_exi_options *options) {
    sxml_item document = {.representation = 0};
    sxml_status status = parse_text(reading.test_fixture, reading.header_text, &document);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_options_reader *options_reader = NULL;
    const sxml_allocator allocator = fixture_allocator(reading.test_fixture);
    status = sxml_exi_options_reader_create(
        allocator,
        &(sxml_exi_options_reader_configuration){
            .allocator = allocator, .name_pool = sxml_processor_get_name_pool(reading.test_fixture->processor)},
        &options_reader);
    sxml_diagnostic diagnostic = {.description = {.bytes = NULL, .byte_count = 0}, .location = {.line_number = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_options_reader_read(
            options_reader,
            &(sxml_exi_options_read_request){.header_element = document_element_of(document),
                                             .diagnostic = &diagnostic},
            options);
    }
    /* A refusal describes itself. */
    if (status == SXML_STATUS_MALFORMED_INPUT && diagnostic.description.byte_count == 0) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_exi_options_reader_destroy(options_reader);
    sxml_item_release(document);
    return status;
}

static void test_options_reader_reads_each_option(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true}, &test_fixture) == SXML_STATUS_OK);
    sxml_exi_options options;
    memset(&options, 0xFF, sizeof(options));
    /* The empty header: the defaults, a zero-initialized struct. */
    SXML_TEST_EXPECT(state, read_options((options_reading){&test_fixture, HEADER_START HEADER_END}, &options) ==
                                SXML_STATUS_OK);
    const sxml_exi_options defaults = {.alignment = SXML_EXI_ALIGNMENT_BIT_PACKED};
    SXML_TEST_EXPECT(state, options.alignment == defaults.alignment && !options.is_strict && !options.is_fragment &&
                                !options.has_block_size && !options.fidelity.preserves_comments &&
                                options.schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_ABSENT &&
                                options.datatype_representation_map.datatype_representation_count == 0);
    SXML_TEST_EXPECT(state, read_options((options_reading){&test_fixture, HEADER_START
                                                           "<lesscommon><uncommon><alignment><byte/>"
                                                           "</alignment></uncommon></lesscommon>" HEADER_END},
                                         &options) == SXML_STATUS_OK &&
                                options.alignment == SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT);
    SXML_TEST_EXPECT(state, read_options((options_reading){&test_fixture, HEADER_START
                                                           "<lesscommon><uncommon><alignment><pre-compress/>"
                                                           "</alignment></uncommon></lesscommon>" HEADER_END},
                                         &options) == SXML_STATUS_OK &&
                                options.alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION);
    SXML_TEST_EXPECT(state, read_options((options_reading){&test_fixture, HEADER_START
                                                           "<common><compression/><fragment/></common>" HEADER_END},
                                         &options) == SXML_STATUS_OK &&
                                options.alignment == SXML_EXI_ALIGNMENT_COMPRESSION && options.is_fragment);
    /* Every uncommon option, with whitespace, a comment and a user-defined
       option of another namespace first. */
    SXML_TEST_EXPECT(state,
                     read_options((options_reading){&test_fixture, HEADER_START
                                                    "<lesscommon> <uncommon><user:option xmlns:user='urn:user'>"
                                                    "<anything/></user:option><!-- c --><selfContained/>"
                                                    "<valueMaxLength> 10 </valueMaxLength>"
                                                    "<valuePartitionCapacity>0</valuePartitionCapacity>"
                                                    "<datatypeRepresentationMap xmlns:xs='http://www.w3.org/2001/"
                                                    "XMLSchema'><xs:decimal/><string/>"
                                                    "</datatypeRepresentationMap>"
                                                    "<datatypeRepresentationMap xmlns:xs='http://www.w3.org/2001/"
                                                    "XMLSchema'><xs:date/><dateTime/>"
                                                    "</datatypeRepresentationMap></uncommon>"
                                                    "<blockSize>+5</blockSize></lesscommon>" HEADER_END},
                                  &options) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, options.allows_self_contained && options.has_value_max_length &&
                                options.value_max_length == 10 && options.has_value_partition_capacity &&
                                options.value_partition_capacity == 0 && options.has_block_size &&
                                options.block_size == 5 &&
                                options.datatype_representation_map.datatype_representation_count == 2);
    SXML_TEST_EXPECT(
        state, read_options((options_reading){&test_fixture, HEADER_START "<lesscommon><preserve><dtd/><prefixes/>"
                                                                          "<lexicalValues/><comments/><pis/></preserve>"
                                                                          "</lesscommon>" HEADER_END},
                            &options) == SXML_STATUS_OK &&
                   options.fidelity.preserves_dtd && options.fidelity.preserves_prefixes &&
                   options.fidelity.preserves_lexical_values && options.fidelity.preserves_comments &&
                   options.fidelity.preserves_processing_instructions);
    /* strict allows lexicalValues. */
    SXML_TEST_EXPECT(state, read_options((options_reading){&test_fixture,
                                                           HEADER_START "<lesscommon><preserve><lexicalValues/>"
                                                                        "</preserve></lesscommon><strict/>" HEADER_END},
                                         &options) == SXML_STATUS_OK &&
                                options.is_strict);
    /* schemaId: text, empty, nil. */
    SXML_TEST_EXPECT(state,
                     read_options((options_reading){&test_fixture, HEADER_START
                                                    "<common><schemaId>urn:s<!-- -->1</schemaId></common>" HEADER_END},
                                  &options) == SXML_STATUS_OK &&
                         options.schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_TEXT &&
                         options.schema_identifier.text.byte_count == 6);
    SXML_TEST_EXPECT(
        state, read_options((options_reading){&test_fixture, HEADER_START "<common><schemaId/></common>" HEADER_END},
                            &options) == SXML_STATUS_OK &&
                   options.schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES);
    SXML_TEST_EXPECT(state, read_options((options_reading){&test_fixture, HEADER_START
                                                           "<common><schemaId xmlns:xsi='http://www.w3.org/2001/"
                                                           "XMLSchema-instance' xsi:nil='true'/></common>" HEADER_END},
                                         &options) == SXML_STATUS_OK &&
                                options.schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_NONE);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_options_reader_refuses_invalid_documents(sxml_test_state *state) {
    static const char *const refused[] = {
        /* Not exi:header. */
        "<header/>",
        "<options xmlns='http://www.w3.org/2009/exi'/>",
        /* Not in the schema, out of order, repeated, text. */
        HEADER_START "<unknown/>" HEADER_END,
        HEADER_START "<common/><lesscommon/>" HEADER_END,
        HEADER_START "<strict/><strict/>" HEADER_END,
        HEADER_START "text" HEADER_END,
        HEADER_START "<lesscommon><preserve><comments/><dtd/></preserve></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><alignment><byte/></alignment><user:option xmlns:user='urn:user'/>"
                     "</uncommon></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><option xmlns=''/></lesscommon>" HEADER_END,
        /* Empty elements are empty; alignment holds one choice. */
        HEADER_START "<strict>yes</strict>" HEADER_END,
        HEADER_START "<common><compression><byte/></compression></common>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><alignment/></uncommon></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><alignment><byte/><pre-compress/></alignment></uncommon>"
                     "</lesscommon>" HEADER_END,
        /* Attributes, xsi:nil only on schemaId and only empty. */
        "<header xmlns='http://www.w3.org/2009/exi' version='1'/>",
        HEADER_START "<strict xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:nil='true'/>" HEADER_END,
        HEADER_START "<common><schemaId xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                     "xsi:nil='true'>s</schemaId></common>" HEADER_END,
        HEADER_START "<common><schemaId xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
                     "xsi:nil='maybe'/></common>" HEADER_END,
        /* Numbers. */
        HEADER_START "<lesscommon><blockSize>0</blockSize></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><blockSize>x</blockSize></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><blockSize>4294967296</blockSize></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><blockSize>-1</blockSize></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><blockSize></blockSize></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><valueMaxLength><a/></valueMaxLength></uncommon>"
                     "</lesscommon>" HEADER_END,
        /* datatypeRepresentationMap: two elements, a type of another
           namespace, a built-in representation. */
        HEADER_START "<lesscommon><uncommon><datatypeRepresentationMap xmlns:xs='http://www.w3.org/2001/"
                     "XMLSchema'><xs:decimal/></datatypeRepresentationMap></uncommon></lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><datatypeRepresentationMap xmlns:xs='http://www.w3.org/2001/"
                     "XMLSchema'><xs:decimal/><notADatatype/></datatypeRepresentationMap></uncommon>"
                     "</lesscommon>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><datatypeRepresentationMap><decimal/><string/>"
                     "</datatypeRepresentationMap></uncommon></lesscommon>" HEADER_END,
        /* EXI 1.0, 5.4: options that cannot go together. */
        HEADER_START "<lesscommon><uncommon><alignment><byte/></alignment></uncommon></lesscommon>"
                     "<common><compression/></common>" HEADER_END,
        HEADER_START "<lesscommon><preserve><comments/></preserve></lesscommon><strict/>" HEADER_END,
        HEADER_START "<lesscommon><preserve><dtd/></preserve></lesscommon><strict/>" HEADER_END,
        HEADER_START "<lesscommon><preserve><prefixes/></preserve></lesscommon><strict/>" HEADER_END,
        HEADER_START "<lesscommon><preserve><pis/></preserve></lesscommon><strict/>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><selfContained/></uncommon></lesscommon><strict/>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><selfContained/></uncommon></lesscommon>"
                     "<common><compression/></common>" HEADER_END,
        HEADER_START "<lesscommon><uncommon><alignment><pre-compress/></alignment><selfContained/>"
                     "</uncommon></lesscommon>" HEADER_END,
    };
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true}, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(refused); case_index += 1) {
        sxml_exi_options options;
        memset(&options, 0, sizeof(options));
        options.block_size = 77;
        const sxml_status status = read_options((options_reading){&test_fixture, refused[case_index]}, &options);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_MALFORMED_INPUT);
        /* On failure *options is untouched. */
        SXML_TEST_EXPECT(state, options.block_size == 77);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A read's views stay valid after its tree is gone, until the next read;
   a schemaId's texts around a comment are its value. */
static void test_options_reader_keeps_its_views(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true}, &test_fixture) == SXML_STATUS_OK);
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_text(&test_fixture,
                                        HEADER_START "<common><schemaId>urn:<!-- -->kept</schemaId>"
                                                     "</common>" HEADER_END,
                                        &document) == SXML_STATUS_OK);
    const sxml_allocator allocator = fixture_allocator(&test_fixture);
    sxml_exi_options_reader *options_reader = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_exi_options_reader_create(
                   allocator,
                   &(sxml_exi_options_reader_configuration){
                       .allocator = allocator, .name_pool = sxml_processor_get_name_pool(test_fixture.processor)},
                   &options_reader) == SXML_STATUS_OK);
    sxml_exi_options options;
    memset(&options, 0, sizeof(options));
    SXML_TEST_EXPECT(
        state, sxml_exi_options_reader_read(options_reader,
                                            &(sxml_exi_options_read_request){
                                                .header_element = document_element_of(document), .diagnostic = NULL},
                                            &options) == SXML_STATUS_OK);
    sxml_item_release(document);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(options.schema_identifier.text, TEXT("urn:kept")));
    /* Invalid arguments. */
    SXML_TEST_EXPECT(state,
                     sxml_exi_options_reader_read(options_reader, NULL, &options) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_exi_options_reader_create(
                                allocator, &(sxml_exi_options_reader_configuration){.allocator = allocator},
                                &(sxml_exi_options_reader *){NULL}) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_exi_options_reader_destroy(options_reader);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------ Functions */

static void test_functions_serialize_and_parse(sxml_test_state *state) {
    static const char *const cases[][2] = {
        /* A round trip, and the empty stream. */
        {PROLOG "exi:parse(exi:serialize(<a b='1'>x<c/></a>)) => serialize()", "\"<a b=\"\"1\"\">x<c/></a>\""},
        {PROLOG "exi:parse(())", ""},
        {PROLOG "exi:serialize(<a/>) instance of xs:base64Binary", "true()"},
        /* The cookie, "$EXI" (EXI 1.0, 5.1). */
        {PROLOG "starts-with(string(xs:hexBinary(exi:serialize(<a/>, {'include-cookie': true()}))), '24455849')",
         "true()"},
        /* Options: byte alignment, given to both sides. */
        {PROLOG "let $o := <header xmlns='http://www.w3.org/2009/exi'><lesscommon><uncommon><alignment><byte/>"
                "</alignment></uncommon></lesscommon></header> return exi:parse(exi:serialize(<a>text</a>, "
                "{'options': $o}), {'options': $o}) => serialize()",
         "\"<a>text</a>\""},
        /* Comments kept with Preserve.comments on both sides. */
        {PROLOG "let $o := <header xmlns='http://www.w3.org/2009/exi'><lesscommon><preserve><comments/>"
                "</preserve></lesscommon></header> return exi:parse(exi:serialize(<a><!--c--></a>, "
                "{'options': $o}), {'options': $o}) => serialize()",
         "\"<a><!--c--></a>\""},
        /* Sequence normalization (Serialization 4.0, section 2), into a
           fragment (EXI 1.0, 8.4.2). */
        {PROLOG "let $o := <header xmlns='http://www.w3.org/2009/exi'><common><fragment/></common></header> "
                "return exi:parse(exi:serialize(([<a/>], document { <b/> }), {'options': $o}), {'options': $o}) "
                "=> serialize()",
         "\"<a/><b/>\""},
        /* Text outside any element is no EXI document (EXI 1.0, 8.4.1). */
        {PROLOG "exi:serialize((<a/>, 'x', 1))", "!exi:unrepresentable"},
        /* Refusals. */
        {PROLOG "exi:serialize(<a/>, {'options': <header/>})", "!exi:invalid-options"},
        {PROLOG "exi:serialize(<a/>, {'options': <header xmlns='http://www.w3.org/2009/exi'><strict/><strict/>"
                "</header>})",
         "!exi:invalid-options"},
        {PROLOG "exi:serialize(<a/>, {'include-cookie': 'yes'})", "!XPTY0004"},
        {PROLOG "exi:serialize(attribute a {1})", "!SENR0001"},
        {PROLOG "exi:parse(xs:base64Binary('AAAA'))", "!exi:malformed"},
        {PROLOG "exi:parse(xs:base64Binary(''))", "!exi:malformed"},
        {PROLOG "exi:parse(exi:serialize(<a/>, {'options': <header xmlns='http://www.w3.org/2009/exi'><common>"
                "<schemaId>urn:none</schemaId></common></header>, 'include-options': true()}))",
         "!exi:unsupported"},
        /* fn:serialize returns a string: the exi method is refused (F&O
           4.0, 17.2.3). */
        {PROLOG "serialize(<a/>, {'method': xs:QName('exi:exi')})", "!SEPM0016"},
        {PROLOG "serialize(<a/>, {'method': QName('urn:sxml:other', 'exi')})", "!SEPM0016"},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

static void test_functions_are_absent_without_the_feature(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = false}, &test_fixture) == SXML_STATUS_OK);
    expect_query(state, &test_fixture, (const char *const[2]){PROLOG "exi:serialize(<a/>)", "!XPST0017"});
    expect_query(state, &test_fixture, (const char *const[2]){PROLOG "exi:parse(())", "!XPST0017"});
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_optional_feature_get_name(sxml_exi_feature()), TEXT("exi")));
    bool is_listed = false;
    const sxml_optional_feature_view all = sxml_all_optional_features();
    for (size_t feature_index = 0; feature_index < all.optional_feature_count; feature_index += 1) {
        is_listed = is_listed || all.optional_features[feature_index].definition == sxml_exi_feature().definition;
    }
    SXML_TEST_EXPECT(state, is_listed);
}

/* --------------------------------------------------------------- Method */

/* The output method from an output declaration writes what exi:serialize
   returns, with the exi parameters the prolog declares. */
static void test_method_from_output_declarations(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true}, &test_fixture) == SXML_STATUS_OK);
    sxml_error_record error_record;
    SXML_TEST_EXPECT(state, run_query(&test_fixture,
                                      (query_run){.query_text = PROLOG
                                                  "xs:hexBinary(exi:serialize(<a x='1'>t</a>, {'include-cookie': "
                                                  "true()})) => string()",
                                                  .uses_adaptive = true},
                                      &error_record) == SXML_STATUS_OK);
    uint8_t expected_hex[128];
    const sxml_byte_view hex = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
    SXML_TEST_REQUIRE(state, hex.byte_count < sizeof(expected_hex));
    memcpy(expected_hex, hex.bytes, hex.byte_count);
    const size_t expected_hex_count = hex.byte_count;
    SXML_TEST_EXPECT(
        state, run_query(&test_fixture,
                         (query_run){.query_text = PROLOG "declare option output:method 'exi:exi'; "
                                                          "declare option exi:include-cookie 'yes'; <a x='1'>t</a>",
                                     .uses_adaptive = false},
                         &error_record) == SXML_STATUS_OK);
    const sxml_byte_view bytes = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
    /* The adaptive output is "\"HEX\"", two hex digits a byte. */
    SXML_TEST_EXPECT(state, expected_hex_count == bytes.byte_count * 2 + 2);
    SXML_TEST_EXPECT(state, bytes.byte_count >= 4 && memcmp(bytes.bytes, "$EXI", 4) == 0);
    static const char hex_digits[] = "0123456789ABCDEF";
    bool is_same = expected_hex_count == bytes.byte_count * 2 + 2;
    for (size_t byte_index = 0; is_same && byte_index < bytes.byte_count; byte_index += 1) {
        is_same = expected_hex[1 + byte_index * 2] == (uint8_t)hex_digits[bytes.bytes[byte_index] >> 4U] &&
                  expected_hex[2 + byte_index * 2] == (uint8_t)hex_digits[bytes.bytes[byte_index] & 0xFU];
    }
    SXML_TEST_EXPECT(state, is_same);
    /* Refusals of the declarations: an unknown exi parameter, exi:options
       as text, another implementation-defined method. */
    SXML_TEST_EXPECT(state, run_query(&test_fixture,
                                      (query_run){.query_text = PROLOG "declare option exi:unknown 'yes'; 1",
                                                  .uses_adaptive = false},
                                      &error_record) == SXML_STATUS_STATIC_ERROR &&
                                sxml_byte_view_equals(error_record.code.local_name, TEXT("SEPM0016")));
    SXML_TEST_EXPECT(state, run_query(&test_fixture,
                                      (query_run){.query_text = PROLOG "declare option exi:options '<a/>'; 1",
                                                  .uses_adaptive = false},
                                      &error_record) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, run_query(&test_fixture,
                                      (query_run){.query_text = "declare namespace o = 'urn:other'; "
                                                                "declare option output:method 'o:exi'; 1",
                                                  .uses_adaptive = false},
                                      &error_record) == SXML_STATUS_STATIC_ERROR &&
                                sxml_byte_view_equals(error_record.code.local_name, TEXT("SEPM0016")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* The parameter reader takes the method and its parameters from a map and
   from a parameter document. */
static void test_parameter_reader_reads_the_exi_parameters(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true}, &test_fixture) == SXML_STATUS_OK);
    static const char *const queries[] = {
        PROLOG "map { 'method': xs:QName('exi:exi'), xs:QName('exi:include-cookie'): true(), "
               "xs:QName('exi:self-contained-elements'): xs:QName('a'), "
               "xs:QName('exi:options'): <header xmlns='http://www.w3.org/2009/exi'><lesscommon><uncommon>"
               "<alignment><byte/></alignment><selfContained/></uncommon></lesscommon></header> }",
        "<output:serialization-parameters xmlns:output='http://www.w3.org/2010/xslt-xquery-serialization' "
        "xmlns:exi='urn:sxml:exi'><output:method value='exi:exi'/><exi:include-cookie value='yes'/>"
        "<exi:self-contained-elements value='a'/><exi:options><header xmlns='http://www.w3.org/2009/exi'>"
        "<lesscommon><uncommon><alignment><byte/></alignment><selfContained/></uncommon></lesscommon></header>"
        "</exi:options></output:serialization-parameters>",
    };
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture.processor);
    for (size_t query_index = 0; query_index < SXML_TEST_COUNT(queries); query_index += 1) {
        sxml_error_record error_record;
        memset(&error_record, 0, sizeof(error_record));
        sxml_xquery_query *query = NULL;
        SXML_TEST_REQUIRE(state, sxml_xquery_query_create(fixture_allocator(&test_fixture),
                                                          &(sxml_xquery_query_configuration){
                                                              .processor = test_fixture.processor,
                                                              .expression_text = text_of(queries[query_index]),
                                                              .error_record = &error_record},
                                                          &query) == SXML_STATUS_OK);
        sxml_sequence options = sxml_sequence_empty();
        SXML_TEST_EXPECT(
            state, sxml_xquery_query_evaluate(query, &(sxml_xquery_evaluation_request){.error_record = &error_record},
                                              &options) == SXML_STATUS_OK);
        sxml_serialization_parameter_reader *parameter_reader = NULL;
        SXML_TEST_REQUIRE(state, sxml_serialization_parameter_reader_create(
                                     fixture_allocator(&test_fixture),
                                     &(sxml_serialization_parameter_reader_configuration){
                                         .allocator = fixture_allocator(&test_fixture), .item_heap = item_heap},
                                     &parameter_reader) == SXML_STATUS_OK);
        sxml_serialization_parameters parameters;
        memset(&parameters, 0, sizeof(parameters));
        SXML_TEST_EXPECT(
            state, sxml_serialization_parameter_reader_read(
                       parameter_reader,
                       &(sxml_serialization_parameter_read_request){.options = options, .error_record = &error_record},
                       &parameters) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, parameters.serialization_method == SXML_SERIALIZATION_METHOD_EXI);
        SXML_TEST_REQUIRE(state, parameters.exi_parameters != NULL);
        const sxml_exi_serialization_parameters *exi = parameters.exi_parameters;
        SXML_TEST_EXPECT(state, exi->header_options.includes_cookie && !exi->header_options.includes_options &&
                                    exi->options.alignment == SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT &&
                                    exi->options.allows_self_contained &&
                                    exi->self_contained_elements.element_name_count == 1);
        /* The method writes the stream. */
        sxml_output_buffer_clear(test_fixture.output_buffer);
        sxml_item document = {.representation = 0};
        SXML_TEST_EXPECT(state, parse_text(&test_fixture, "<a><b/></a>", &document) == SXML_STATUS_OK);
        const sxml_sequence value = sxml_sequence_from_item(document);
        SXML_TEST_EXPECT(state,
                         sxml_serialize_sequence(
                             item_heap,
                             &(sxml_serialization_request){
                                 .value = value, .serialization_parameters = parameters, .error_record = &error_record},
                             sxml_output_buffer_as_output_stream(test_fixture.output_buffer)) == SXML_STATUS_OK);
        const sxml_byte_view bytes = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
        SXML_TEST_EXPECT(state, bytes.byte_count > 4 && memcmp(bytes.bytes, "$EXI", 4) == 0);
        sxml_sequence_release(value);
        sxml_serialization_parameter_reader_destroy(parameter_reader);
        sxml_sequence_release(options);
        sxml_xquery_query_destroy(query);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------- Parsing and schemas */

static void test_parse_exi_document(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true}, &test_fixture) == SXML_STATUS_OK);
    sxml_error_record error_record;
    SXML_TEST_REQUIRE(state, run_query(&test_fixture,
                                       (query_run){.query_text = PROLOG "declare option output:method 'exi:exi'; "
                                                                        "<doc><item n='1'>one</item></doc>",
                                                   .uses_adaptive = false},
                                       &error_record) == SXML_STATUS_OK);
    const sxml_byte_view view = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
    uint8_t stream[256];
    SXML_TEST_REQUIRE(state, view.byte_count <= sizeof(stream));
    memcpy(stream, view.bytes, view.byte_count);
    const sxml_byte_view input = {.bytes = stream, .byte_count = view.byte_count};
    sxml_item document = {.representation = 0};
    sxml_diagnostic diagnostic = {.description = {.bytes = NULL, .byte_count = 0}, .location = {.line_number = 0}};
    SXML_TEST_EXPECT(
        state, sxml_processor_parse_exi_document(test_fixture.processor,
                                                 &(sxml_exi_decode_request){.input = input, .diagnostic = &diagnostic},
                                                 &document) == SXML_STATUS_OK);
    sxml_output_buffer_clear(test_fixture.output_buffer);
    SXML_TEST_EXPECT(state, sxml_serialize_sequence(
                                sxml_processor_get_item_heap(test_fixture.processor),
                                &(sxml_serialization_request){
                                    .value = sxml_sequence_from_item(document),
                                    .serialization_parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_XML,
                                                                 .omit_xml_declaration = SXML_SERIALIZATION_FLAG_YES},
                                    .error_record = &error_record},
                                sxml_output_buffer_as_output_stream(test_fixture.output_buffer)) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                                  TEXT("<doc><item n=\"1\">one</item></doc>")));
    sxml_item_release(document);
    /* A stream that ends early, with its diagnostic; nothing is made. */
    document = (sxml_item){.representation = 0};
    SXML_TEST_EXPECT(
        state, sxml_processor_parse_exi_document(
                   test_fixture.processor,
                   &(sxml_exi_decode_request){.input = {.bytes = stream, .byte_count = 1}, .diagnostic = &diagnostic},
                   &document) != SXML_STATUS_OK &&
                   document.representation == 0);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    /* Without the feature. */
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = false}, &test_fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_processor_parse_exi_document(test_fixture.processor,
                                                              &(sxml_exi_decode_request){.input = input},
                                                              &document) == SXML_STATUS_UNSUPPORTED_FEATURE);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A resolver that serves one schema document for the schemaId "urn:s"
   and counts how often it is asked. */
typedef struct counting_resolver {
    size_t request_count;
} counting_resolver;

static sxml_status resolve_counted(void *state, const sxml_resource_request *request, sxml_resource *resource) {
    counting_resolver *resolver = state;
    if (request->resource_kind != SXML_RESOURCE_KIND_EXI_SCHEMA ||
        !sxml_byte_view_equals(request->uri_reference, TEXT("urn:s"))) {
        return SXML_STATUS_OK;
    }
    resolver->request_count += 1;
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = TEXT("<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                             "<xs:element name='a' type='xs:string'/></xs:schema>");
    return SXML_STATUS_OK;
}

static const sxml_resource_resolver_operations counting_resolver_operations = {.resolve = resolve_counted,
                                                                               .release = NULL};

static void test_schema_resolver_keeps_compiled_schemas(sxml_test_state *state) {
    counting_resolver resolver = {.request_count = 0};
    fixture test_fixture;
    SXML_TEST_REQUIRE(
        state, fixture_create(&(fixture_options){.names_feature = true,
                                                 .resource_resolver = {.state = &resolver,
                                                                       .operations = &counting_resolver_operations}},
                              &test_fixture) == SXML_STATUS_OK);
    const sxml_exi_schema_resolver schema_resolver = sxml_processor_exi_schema_resolver(test_fixture.processor);
    SXML_TEST_REQUIRE(state, schema_resolver.operations != NULL && schema_resolver.operations->resolve != NULL);
    const sxml_exi_schema_identifier identifier = {.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_TEXT,
                                                   .text = TEXT("urn:s")};
    const sxml_schema *first = NULL;
    const sxml_schema *second = NULL;
    SXML_TEST_EXPECT(state,
                     schema_resolver.operations->resolve(schema_resolver.state, &identifier, &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, schema_resolver.operations->resolve(schema_resolver.state, &identifier, &second) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, first != NULL && first == second && resolver.request_count == 1);
    /* A schemaId the resolver does not serve: no schema. */
    const sxml_schema *missing = first;
    SXML_TEST_EXPECT(state, schema_resolver.operations->resolve(
                                schema_resolver.state,
                                &(sxml_exi_schema_identifier){.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_TEXT,
                                                              .text = TEXT("urn:missing")},
                                &missing) == SXML_STATUS_OK &&
                                missing == NULL);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static const sxml_test_case exi_processor_test_cases[] = {
    {.name = "options_reader_reads_each_option", .function = test_options_reader_reads_each_option},
    {.name = "options_reader_refuses_invalid_documents", .function = test_options_reader_refuses_invalid_documents},
    {.name = "options_reader_keeps_its_views", .function = test_options_reader_keeps_its_views},
    {.name = "functions_serialize_and_parse", .function = test_functions_serialize_and_parse},
    {.name = "functions_are_absent_without_the_feature", .function = test_functions_are_absent_without_the_feature},
    {.name = "method_from_output_declarations", .function = test_method_from_output_declarations},
    {.name = "parameter_reader_reads_the_exi_parameters", .function = test_parameter_reader_reads_the_exi_parameters},
    {.name = "parse_exi_document", .function = test_parse_exi_document},
    {.name = "schema_resolver_keeps_compiled_schemas", .function = test_schema_resolver_keeps_compiled_schemas},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "exi_processor",
            .cases = exi_processor_test_cases,
            .case_count = SXML_TEST_COUNT(exi_processor_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
