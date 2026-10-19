/*
 * Tests for streamed evaluation (src/evaluator_stream.c,
 * src/xslt_lower_stream.c; docs/DECISIONS.md, D219; packet 7.6b) through
 * the public API: xsl:source-document with streamable="yes" gives what
 * the same stylesheet gives unstreamed (child and descendant steps,
 * predicates and positions, nested stream points with shallow windows,
 * subtrees copied from full windows, text, comments and processing
 * instructions, namespaces, whitespace stripping, empty elements, an
 * unstreamed document node from the resolver); and a large generated
 * document streams under an allocator limit far below what its tree
 * needs, which the unstreamed run exceeds (bounded memory, 18.1). Every
 * case checks for leaked cells and allocations.
 *
 * Golden file: tests/xslt_test.c.
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xslt.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* A stylesheet whose template main reads in.xml, streamable or not. */
#define STREAMED(declarations, streamable, body)                                                   \
    "<xsl:stylesheet version='3.0' " XSL_NAMESPACE                                                 \
    " xmlns:p='urn:p' exclude-result-prefixes='#all'>" declarations                                \
    "<xsl:template name='main'><out><xsl:source-document streamable='" streamable                  \
    "' href='in.xml'>" body "</xsl:source-document></out></xsl:template></xsl:stylesheet>"

/* --------------------------------------------------------- Limited memory */

/* An allocator that counts the bytes it holds and fails beyond a limit:
   the evidence of bounded memory. Each block has a header in front. */
typedef struct limited_memory {
    sxml_allocator backing;
    size_t limit_byte_count;
    size_t live_byte_count;
    size_t peak_byte_count;
    uint64_t live_block_count;
} limited_memory;

#define LIMITED_HEADER_BYTE_COUNT 32U

typedef struct limited_header {
    void *block;
    size_t byte_count;
} limited_header;

static sxml_status limited_allocate(void *state, sxml_allocation_request request, void **memory) {
    limited_memory *limited = state;
    if (request.alignment > LIMITED_HEADER_BYTE_COUNT ||
        request.byte_count > SIZE_MAX - LIMITED_HEADER_BYTE_COUNT) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    if (limited->limit_byte_count != 0 &&
        limited->live_byte_count + request.byte_count > limited->limit_byte_count) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    void *block = NULL;
    const sxml_status status = sxml_allocator_allocate(
        limited->backing,
        (sxml_allocation_request){.byte_count = request.byte_count + LIMITED_HEADER_BYTE_COUNT,
                                  .alignment = LIMITED_HEADER_BYTE_COUNT},
        &block);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = block;
    const limited_header header = {.block = block, .byte_count = request.byte_count};
    memcpy(bytes, &header, sizeof(header));
    limited->live_byte_count += request.byte_count;
    limited->live_block_count += 1;
    if (limited->live_byte_count > limited->peak_byte_count) {
        limited->peak_byte_count = limited->live_byte_count;
    }
    *memory = bytes + LIMITED_HEADER_BYTE_COUNT;
    return SXML_STATUS_OK;
}

static void limited_release(void *state, void *memory) {
    limited_memory *limited = state;
    if (memory == NULL) {
        return;
    }
    limited_header header;
    memcpy(&header, (uint8_t *)memory - LIMITED_HEADER_BYTE_COUNT, sizeof(header));
    limited->live_byte_count -= header.byte_count;
    limited->live_block_count -= 1;
    sxml_allocator_release(limited->backing, header.block);
}

static const sxml_allocator_operations limited_operations = {.allocate = limited_allocate,
                                                             .release = limited_release};

/* ------------------------------------------------------------- Resolver */

/* What the resolver serves at http://example.com/x/in.xml. */
typedef struct served_document {
    sxml_byte_view content;
    /* Deliver the document as a node the host parsed, not as bytes. */
    bool is_node;
    sxml_item node;
} served_document;

static sxml_status resolve_input(void *state, const sxml_resource_request *request,
                                 sxml_resource *resource) {
    served_document *served = state;
    if (request->resource_kind != SXML_RESOURCE_KIND_XML_DOCUMENT ||
        !sxml_byte_view_equals(request->absolute_uri, TEXT("http://example.com/x/in.xml"))) {
        return SXML_STATUS_OK;
    }
    if (served->is_node) {
        sxml_item_retain(served->node);
        resource->resource_form = SXML_RESOURCE_FORM_DOCUMENT_NODE;
        resource->document_node = served->node;
        return SXML_STATUS_OK;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = served->content;
    resource->resource_uri = request->absolute_uri;
    return SXML_STATUS_OK;
}

static const sxml_resource_resolver_operations input_resolver_operations = {
    .resolve = resolve_input, .release = NULL};

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    limited_memory memory;
    served_document served;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    sxml_error_record error_record;
    /* The option of XSLT 4.0, 19.10 for the stylesheets run_main makes. */
    bool falls_back_to_unstreamed;
} fixture;

static sxml_allocator fixture_allocator(fixture *test_fixture) {
    return (sxml_allocator){.state = &test_fixture->memory, .operations = &limited_operations};
}

/* Makes the processor; limit_byte_count 0 is no limit. */
static sxml_status fixture_create(fixture *test_fixture, sxml_byte_view content) {
    memset(test_fixture, 0, sizeof(*test_fixture));
    test_fixture->memory.backing = sxml_default_allocator();
    test_fixture->served =
        (served_document){.content = content, .is_node = false, .node = {.representation = 0}};
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    sxml_status status =
        sxml_processor_create(allocator,
                              &(sxml_processor_configuration){
                                  .allocator = allocator,
                                  .resource_resolver = {.state = &test_fixture->served,
                                                        .operations = &input_resolver_operations}},
                              &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when nothing leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_release(test_fixture->served.node);
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    return has_no_leak && test_fixture->memory.live_block_count == 0;
}

static sxml_name_identifier main_name(const fixture *test_fixture) {
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(test_fixture->processor),
            &(sxml_name_request){.namespace_uri = {.index = 0}, .local_name = TEXT("main")},
            &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

/* Compiles stylesheet_text and calls template main, serialized into the
   output buffer (cleared first). */
static sxml_status run_main(fixture *test_fixture, sxml_byte_view stylesheet_text) {
    memset(&test_fixture->error_record, 0, sizeof(test_fixture->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_text = stylesheet_text,
            .static_context = {.static_base_uri = TEXT("http://example.com/x/main.xsl")},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &test_fixture->error_record,
            .falls_back_to_unstreamed = test_fixture->falls_back_to_unstreamed},
        &stylesheet);
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof(dynamic_context));
        sxml_sequence principal = sxml_sequence_empty();
        status = sxml_xslt_stylesheet_transform(
            stylesheet,
            &(sxml_xslt_transformation_request){
                .dynamic_context = dynamic_context,
                .initial_match_selection = sxml_sequence_empty(),
                .base_output_uri = TEXT("http://example.com/out/main.xml"),
                .output_definition = NULL,
                .output_stream = sxml_output_buffer_as_output_stream(test_fixture->output_buffer),
                .error_record = &test_fixture->error_record,
                .initial_template_name = main_name(test_fixture),
                .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
            &principal);
        sxml_sequence_release(principal);
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

/* ------------------------------------------------- Streamed = unstreamed */

/* A stylesheet body, run streamed and unstreamed over document, and the
   output both must give. */
typedef struct stream_case {
    const char *case_name;
    const char *streamed_text;
    const char *unstreamed_text;
    const char *document_text;
    const char *expected_output;
} stream_case;

#define STREAM_CASE(name, declarations, body, document, expected)                                  \
    {                                                                                              \
        .case_name = (name), .streamed_text = STREAMED(declarations, "yes", body),                 \
        .unstreamed_text = STREAMED(declarations, "no", body), .document_text = (document),        \
        .expected_output = (expected)                                                              \
    }

#define DECLARATION "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"

static const char books[] =
    "<shop a='1'><book id='1' kind='x'><t>A</t><p>3</p></book><!--c--><?pi d?>"
    "<book id='2'><t>B</t><p>5</p></book><book id='3' kind='x'><t>C</t><p>7</p>"
    "<book id='4'><t>D</t></book></book><empty/></shop>";

static const stream_case stream_cases[] = {
    STREAM_CASE(
        "child-path-motionless-body", "",
        "<xsl:for-each select='shop/book'><b n='{position()}' id='{@id}'/></xsl:for-each>", books,
        DECLARATION "<out><b n=\"1\" id=\"1\"/><b n=\"2\" id=\"2\"/><b n=\"3\" id=\"3\"/></out>"),
    STREAM_CASE(
        "rooted-path-copied-subtree", "",
        "<xsl:for-each select='/shop/book[@kind]'><xsl:copy-of select='.'/></xsl:for-each>", books,
        DECLARATION "<out><book id=\"1\" kind=\"x\"><t>A</t><p>3</p></book><book id=\"3\" "
                    "kind=\"x\"><t>C</t><p>7</p><book id=\"4\"><t>D</t></book></book></out>"),
    STREAM_CASE("positional-predicate", "",
                "<xsl:for-each select='shop/book[position() lt 3]'>"
                "<xsl:value-of select='t'/></xsl:for-each>",
                books, DECLARATION "<out>AB</out>"),
    STREAM_CASE("nested-shallow-windows", "",
                "<xsl:for-each select='shop'><s a='{@a}'><xsl:for-each select='book'>"
                "<xsl:value-of select='position(), sum(p)' separator=':'/>;</xsl:for-each>"
                "</s></xsl:for-each>",
                books, DECLARATION "<out><s a=\"1\">1:3;2:5;3:7;</s></out>"),
    STREAM_CASE("descendant-step", "",
                "<xsl:for-each select='//book'><xsl:value-of select='@id, count(ancestor::*)'/>|"
                "</xsl:for-each>",
                books, DECLARATION "<out>1 1|2 1|3 1|4 2|</out>"),
    STREAM_CASE("descendant-shallow", "",
                "<xsl:for-each select='//book'><i n='{position()}' id='{@id}'/></xsl:for-each>",
                books,
                DECLARATION "<out><i n=\"1\" id=\"1\"/><i n=\"2\" id=\"2\"/><i n=\"3\" id=\"3\"/>"
                            "<i n=\"4\" id=\"4\"/></out>"),
    STREAM_CASE(
        "path-position-across-steps", "",
        "<xsl:for-each select='shop/book/t'><xsl:value-of select='position()'/></xsl:for-each>",
        books, DECLARATION "<out>123</out>"),
    STREAM_CASE(
        "other-node-kinds", "",
        "<xsl:for-each select='shop/node()'><xsl:value-of select='name()'/>,</xsl:for-each>", books,
        DECLARATION "<out>book,,pi,book,book,empty,</out>"),
    STREAM_CASE("text-children", "",
                "<xsl:for-each select='shop/book/t/text()'>[<xsl:value-of select='.'/>]"
                "</xsl:for-each>",
                books, DECLARATION "<out>[A][B][C]</out>"),
    STREAM_CASE("empty-elements", "",
                "<xsl:for-each select='shop/empty'><xsl:copy-of select='.'/></xsl:for-each>", books,
                DECLARATION "<out><empty/></out>"),
    STREAM_CASE("namespaces-in-windows", "",
                "<xsl:for-each select='p:r/p:e'><xsl:copy-of select='.'/></xsl:for-each>",
                "<p:r xmlns:p='urn:p' xmlns:q='urn:q'><p:e q:a='1'>x</p:e><p:e/></p:r>",
                DECLARATION "<out><p:e xmlns:p=\"urn:p\" xmlns:q=\"urn:q\" q:a=\"1\">x</p:e>"
                            "<p:e xmlns:p=\"urn:p\" xmlns:q=\"urn:q\"/></out>"),
    STREAM_CASE("whitespace-stripped", "<xsl:strip-space elements='*'/>",
                "<xsl:for-each select='r/node()'>[<xsl:value-of select='.'/>]</xsl:for-each>",
                "<r>\n  <a>1</a>\n  <b> </b>\n  text\n</r>",
                DECLARATION "<out>[1][][\n  text\n]</out>"),
    STREAM_CASE("motionless-document-body", "", "<d><xsl:value-of select='document-uri(.)'/></d>",
                books, DECLARATION "<out><d>http://example.com/x/in.xml</d></out>"),
    STREAM_CASE("unstreamed-shape", "", "<xsl:value-of select='count(//book)'/>", books,
                DECLARATION "<out>4</out>"),
};

static void test_streamed_results_are_unstreamed_results(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(stream_cases); case_index += 1) {
        const stream_case *test_case = &stream_cases[case_index];
        fixture test_fixture;
        sxml_status status = fixture_create(
            &test_fixture, (sxml_byte_view){.bytes = (const uint8_t *)test_case->document_text,
                                            .byte_count = strlen(test_case->document_text)});
        const sxml_byte_view expected = {.bytes = (const uint8_t *)test_case->expected_output,
                                         .byte_count = strlen(test_case->expected_output)};
        const char *texts[2] = {test_case->streamed_text, test_case->unstreamed_text};
        for (size_t text_index = 0; status == SXML_STATUS_OK && text_index < 2; text_index += 1) {
            status = run_main(&test_fixture,
                              (sxml_byte_view){.bytes = (const uint8_t *)texts[text_index],
                                               .byte_count = strlen(texts[text_index])});
            const bool matches =
                status == SXML_STATUS_OK &&
                sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                      expected);
            if (!matches) {
                const sxml_byte_view got = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
                printf("  %s (%s): status %d, got %.*s\n", test_case->case_name,
                       text_index == 0 ? "streamed" : "unstreamed", (int)status,
                       (int)got.byte_count, (const char *)got.bytes);
            }
            SXML_TEST_EXPECT(state, matches);
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

/* A document the resolver gives as a node is read unstreamed (the
   STREAM walks its tree), with the same result. */
static void test_document_node_is_read_as_a_tree(sxml_test_state *state) {
    fixture test_fixture;
    sxml_status status =
        fixture_create(&test_fixture, (sxml_byte_view){.bytes = NULL, .byte_count = 0});
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            test_fixture.processor,
            &(sxml_xml_parse_request){
                .input = {.bytes = (const uint8_t *)books, .byte_count = sizeof(books) - 1},
                .source_name = TEXT("in.xml"),
                .document_uri = TEXT("http://example.com/x/in.xml"),
                .base_uri = TEXT("http://example.com/x/in.xml")},
            &test_fixture.served.node);
        test_fixture.served.is_node = true;
    }
    if (status == SXML_STATUS_OK) {
        status = run_main(&test_fixture,
                          TEXT(STREAMED("", "yes",
                                        "<xsl:for-each select='shop/book[2]'><xsl:copy-of "
                                        "select='t'/></xsl:for-each>")));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                           TEXT(DECLARATION "<out><t>B</t></out>")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A document that is not well-formed is FODC0002, also past the part a
   body read. */
static void test_malformed_document_is_an_error(sxml_test_state *state) {
    fixture test_fixture;
    sxml_status status = fixture_create(&test_fixture, TEXT("<r><a/><b></r>"));
    if (status == SXML_STATUS_OK) {
        status =
            run_main(&test_fixture,
                     TEXT(STREAMED("", "yes", "<xsl:for-each select='r/a'><x/></xsl:for-each>")));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(test_fixture.error_record.code.local_name, TEXT("FODC0002")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A body that is not guaranteed-streamable is XTSE3430, or, with the
   option of 19.10, processed without streaming. */
static void test_fallback_option_processes_unstreamed(sxml_test_state *state) {
    fixture test_fixture;
    sxml_status status = fixture_create(
        &test_fixture, (sxml_byte_view){.bytes = (const uint8_t *)books, .byte_count = sizeof(books) - 1});
    static const char not_streamable[] =
        STREAMED("", "yes", "<xsl:value-of select='count(//book), count(//t)'/>");
    if (status == SXML_STATUS_OK) {
        status = run_main(&test_fixture, TEXT(not_streamable));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(test_fixture.error_record.code.local_name,
                                                  TEXT("XTSE3430")));
    test_fixture.falls_back_to_unstreamed = true;
    status = run_main(&test_fixture, TEXT(not_streamable));
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                TEXT(DECLARATION "<out>4 4</out>")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------- Bounded memory */

/* Records of a generated document: about 80 bytes each (2.4 MB). */
#define LARGE_RECORD_COUNT 30000U

/* The allocator limit for the streamed run: the processor, the compiled
   stylesheet, the windows and the output, not the document. */
#define STREAM_LIMIT_BYTE_COUNT ((size_t)1U * 1024U * 1024U)

/* Writes the document into memory from the default allocator (the input
   the host holds, outside the processor's allocator). */
static sxml_status make_large_document(sxml_byte_view *document, void **memory) {
    const size_t capacity = (size_t)LARGE_RECORD_COUNT * 160U + 64U;
    sxml_status status = sxml_allocator_allocate(
        sxml_default_allocator(), (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
        memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    char *text = *memory;
    size_t length = 0;
    length += (size_t)snprintf(text + length, capacity - length, "<records>\n");
    for (uint32_t record = 1; record <= LARGE_RECORD_COUNT; record += 1) {
        length += (size_t)snprintf(
            text + length, capacity - length,
            "  <record id='%u' hot='%s'><name>item %u</name><price>%u.25</price></record>\n",
            record, record % 10000U == 0 ? "yes" : "no", record, record % 100U);
    }
    length += (size_t)snprintf(text + length, capacity - length, "</records>\n");
    *document = (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = length};
    return SXML_STATUS_OK;
}

static const char large_streamed[] =
    STREAMED("", "yes",
             "<xsl:for-each select='records/record[@hot = \"yes\"]'><r id='{@id}'>"
             "<xsl:value-of select='.'/></r></xsl:for-each>");

static const char large_unstreamed[] =
    STREAMED("", "no",
             "<xsl:for-each select='records/record[@hot = \"yes\"]'><r id='{@id}'>"
             "<xsl:value-of select='.'/></r></xsl:for-each>");

static void test_large_document_streams_in_bounded_memory(sxml_test_state *state) {
    sxml_byte_view document = {.bytes = NULL, .byte_count = 0};
    void *memory = NULL;
    sxml_status status = make_large_document(&document, &memory);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        return;
    }
    fixture test_fixture;
    status = fixture_create(&test_fixture, document);
    test_fixture.memory.limit_byte_count = STREAM_LIMIT_BYTE_COUNT;
    if (status == SXML_STATUS_OK) {
        status = run_main(&test_fixture, TEXT(large_streamed));
    }
    if (status != SXML_STATUS_OK) {
        printf("  streamed: status %d, %.*s: %.*s\n", (int)status,
               (int)test_fixture.error_record.code.local_name.byte_count,
               (const char *)test_fixture.error_record.code.local_name.bytes,
               (int)test_fixture.error_record.diagnostic.description.byte_count,
               (const char *)test_fixture.error_record.diagnostic.description.bytes);
    }
    printf("  document %zu bytes; streamed peak %zu bytes under a limit of %zu\n",
           document.byte_count, test_fixture.memory.peak_byte_count,
           (size_t)STREAM_LIMIT_BYTE_COUNT);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                         TEXT(DECLARATION "<out><r id=\"10000\">item 100000.25</r><r id=\"20000\">"
                                          "item 200000.25</r><r id=\"30000\">item 300000.25</r>"
                                          "</out>")));
    SXML_TEST_EXPECT(state, test_fixture.memory.peak_byte_count < document.byte_count / 4U);
    /* The same transformation unstreamed builds the document's tree,
       which the limit does not hold. */
    sxml_status unstreamed = SXML_STATUS_OK;
    if (status == SXML_STATUS_OK) {
        unstreamed = run_main(&test_fixture, TEXT(large_unstreamed));
    }
    SXML_TEST_EXPECT(state, unstreamed != SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    sxml_allocator_release(sxml_default_allocator(), memory);
}

static const sxml_test_case evaluator_stream_test_cases[] = {
    {.name = "streamed_results_are_unstreamed_results",
     .function = test_streamed_results_are_unstreamed_results},
    {.name = "document_node_is_read_as_a_tree", .function = test_document_node_is_read_as_a_tree},
    {.name = "malformed_document_is_an_error", .function = test_malformed_document_is_an_error},
    {.name = "fallback_option_processes_unstreamed",
     .function = test_fallback_option_processes_unstreamed},
    {.name = "large_document_streams_in_bounded_memory",
     .function = test_large_document_streams_in_bounded_memory},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "evaluator_stream",
            .cases = evaluator_stream_test_cases,
            .case_count = SXML_TEST_COUNT(evaluator_stream_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
