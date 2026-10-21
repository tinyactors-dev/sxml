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

/* As STREAMED, the document's accumulators those use-accumulators names
   (19.2), declared streamable or not. */
#define ACCUMULATED(declarations, streamable, used, body)                                          \
    "<xsl:stylesheet version='3.0' " XSL_NAMESPACE                                                 \
    " xmlns:p='urn:p' exclude-result-prefixes='#all'>" declarations                                \
    "<xsl:template name='main'><out><xsl:source-document streamable='" streamable                  \
    "' use-accumulators='" used "' href='in.xml'>" body                                             \
    "</xsl:source-document></out></xsl:template></xsl:stylesheet>"

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

/* What the resolver serves at http://example.com/x/in.xml, and at
   in2.xml beside it (a second merge input, packet 7.6g). */
typedef struct served_document {
    sxml_byte_view content;
    sxml_byte_view second_content;
    /* Deliver the document as a node the host parsed, not as bytes. */
    bool is_node;
    sxml_item node;
    /* Not 0: deliver in.xml through a byte reader, this many bytes a read
       (D233), from read_offset. */
    size_t chunk_byte_count;
    size_t read_offset;
} served_document;

static sxml_status read_chunk(void *state, sxml_memory_view buffer, sxml_byte_reading *reading) {
    served_document *served = state;
    size_t count = served->content.byte_count - served->read_offset;
    count = count < served->chunk_byte_count ? count : served->chunk_byte_count;
    count = count < buffer.byte_count ? count : buffer.byte_count;
    if (count > 0) {
        memcpy(buffer.bytes, served->content.bytes + served->read_offset, count);
    }
    served->read_offset += count;
    reading->byte_count = count;
    reading->is_end = served->read_offset == served->content.byte_count;
    return SXML_STATUS_OK;
}

static const sxml_byte_reader_operations chunk_reader_operations = {.read = read_chunk};

static sxml_status resolve_input(void *state, const sxml_resource_request *request,
                                 sxml_resource *resource) {
    served_document *served = state;
    if (request->resource_kind == SXML_RESOURCE_KIND_XML_DOCUMENT &&
        sxml_byte_view_equals(request->absolute_uri, TEXT("http://example.com/x/in2.xml"))) {
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = served->second_content;
        resource->resource_uri = request->absolute_uri;
        return SXML_STATUS_OK;
    }
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
    if (served->chunk_byte_count > 0) {
        served->read_offset = 0;
        resource->resource_form = SXML_RESOURCE_FORM_BYTE_READER;
        resource->byte_reader = (sxml_byte_reader){.state = served,
                                                   .operations = &chunk_reader_operations};
        resource->resource_uri = request->absolute_uri;
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

/* Makes the processor with maximum_evaluation_step_count (0: sxml's
   default); limit_byte_count 0 is no limit. */
static sxml_status fixture_create_with_step_limit(fixture *test_fixture, sxml_byte_view content,
                                                  uint64_t maximum_evaluation_step_count) {
    memset(test_fixture, 0, sizeof(*test_fixture));
    test_fixture->memory.backing = sxml_default_allocator();
    test_fixture->served = (served_document){.content = content,
                                             .second_content = {.bytes = NULL, .byte_count = 0},
                                             .is_node = false,
                                             .node = {.representation = 0},
                                             .chunk_byte_count = 0,
                                             .read_offset = 0};
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    sxml_status status =
        sxml_processor_create(allocator,
                              &(sxml_processor_configuration){
                                  .allocator = allocator,
                                  .limits = {.maximum_evaluation_step_count =
                                                 maximum_evaluation_step_count},
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

static sxml_status fixture_create(fixture *test_fixture, sxml_byte_view content) {
    return fixture_create_with_step_limit(test_fixture, content, 0);
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

/* Accumulators computed as the document streams (D231), with the same
   values unstreamed. */
#define ACCUMULATOR_CASE(name, declarations, used, body, document, expected)                       \
    {                                                                                              \
        .case_name = (name), .streamed_text = ACCUMULATED(declarations, "yes", used, body),        \
        .unstreamed_text = ACCUMULATED(declarations, "no", used, body),                            \
        .document_text = (document), .expected_output = (expected)                                 \
    }

/* Counts the books before and after each node; sums the prices. */
#define BOOK_ACCUMULATORS                                                                          \
    "<xsl:accumulator name='n' initial-value='0' streamable='yes'>"                                \
    "<xsl:accumulator-rule match='book' select='$value + 1'/></xsl:accumulator>"                   \
    "<xsl:accumulator name='total' initial-value='0' streamable='yes'>"                            \
    "<xsl:accumulator-rule match='p/text()' select='$value + number(.)'/></xsl:accumulator>"

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
    /* Loops inside a STREAM's body read its counter (position()): the
       loop passes leave them alone (D223, D224): no LET hoisted out of
       the inner for, no filter index for the filter. */
    STREAM_CASE("loops-in-a-streamed-body", "",
                "<xsl:for-each select='shop/book'><xsl:variable name='n' select='position()'/>"
                "<xsl:value-of select='(for $i in 1 to 2 return $n * 10 + $i), "
                "(1 to 20)[string(.) = string($n)]'/>;</xsl:for-each>",
                books, DECLARATION "<out>11 12 1;21 22 2;31 32 3;</out>"),
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
    STREAM_CASE("counted-descendants", "", "<xsl:value-of select='count(//book)'/>", books,
                DECLARATION "<out>4</out>"),
    STREAM_CASE("atomized-sum", "", "<xsl:value-of select='sum(shop/book/p) * 2'/>", books,
                DECLARATION "<out>30</out>"),
    STREAM_CASE("averaged-values", "", "<xsl:value-of select='avg(shop/book/p) * 2'/>", books,
                DECLARATION "<out>10</out>"),
    STREAM_CASE("averaged-nothing", "", "<a><xsl:value-of select='avg(//none)'/></a>", books,
                DECLARATION "<out><a/></out>"),
    STREAM_CASE("atomized-attributes", "",
                "<xsl:value-of select='string-join(/shop/book/@id, \"-\")'/>", books,
                DECLARATION "<out>1-2-3</out>"),
    STREAM_CASE("atomized-descendant-attributes", "",
                "<xsl:value-of select='max(//book/@id)'/>", books, DECLARATION "<out>4</out>"),
    STREAM_CASE("copied-nodes", "", "<xsl:copy-of select='shop/book[2]'/>", books,
                DECLARATION "<out><book id=\"2\"><t>B</t><p>5</p></book></out>"),
    STREAM_CASE("grounded-nodes-in-expression", "",
                "<xsl:value-of select='(shop/book/t)[2] || \"/\" || name(.)'/>", books,
                DECLARATION "<out>B/</out>"),
    STREAM_CASE("simple-map-on-nodes", "",
                "<xsl:value-of select='shop/book ! string(t)' separator=','/>", books,
                DECLARATION "<out>A,B,C</out>"),
    STREAM_CASE("attribute-values-of-a-predicate", "",
                "<xsl:sequence select='count(shop/book[@kind = \"x\"]/@id)'/>", books,
                DECLARATION "<out>2</out>"),
    STREAM_CASE("for-each-with-separator", "",
                "<xsl:for-each select='shop/book' separator=';'>"
                "<xsl:value-of select='t, position()'/></xsl:for-each>",
                books, DECLARATION "<out>A 1;B 2;C 3</out>"),
    STREAM_CASE("streamable-mode-templates",
                "<xsl:mode name='s' streamable='yes' on-no-match='shallow-skip'/>"
                "<xsl:template match='book' mode='s'><b n='{position()}'>"
                "<xsl:apply-templates mode='s'/></b></xsl:template>"
                "<xsl:template match='t' mode='s'><xsl:value-of select='.'/></xsl:template>",
                "<xsl:apply-templates select='shop/book' mode='s'/>", books,
                DECLARATION "<out><b n=\"1\">A</b><b n=\"2\">B</b>"
                            "<b n=\"3\">C<b n=\"3\">D</b></b></out>"),
    STREAM_CASE("iterated-nodes", "",
                "<xsl:iterate select='shop/book'><xsl:param name='n' select='0'/>"
                "<xsl:on-completion><xsl:value-of select='$n'/></xsl:on-completion>"
                "<xsl:next-iteration><xsl:with-param name='n' select='$n + p'/>"
                "</xsl:next-iteration></xsl:iterate>",
                books, DECLARATION "<out>15</out>"),
    STREAM_CASE("text-after-child-elements", "",
                "<xsl:for-each select='r/text()'>[<xsl:value-of select='.'/>]</xsl:for-each>",
                "<r>a<e/>b<e></e>c<e>d</e></r>", DECLARATION "<out>[a][b][c]</out>"),
    STREAM_CASE("counted-text-after-child-elements", "",
                "<xsl:value-of select='count(r/text())'/>", "<r>a<e/>b<e></e>c<e>d</e></r>",
                DECLARATION "<out>3</out>"),
    /* D231: values of shallow windows (before), of full windows' nodes
       (the t of a copied book), after a node's end, and of the document
       node after its end. */
    ACCUMULATOR_CASE("accumulators-of-open-nodes", BOOK_ACCUMULATORS, "#all",
                     "<xsl:for-each select='shop/book'>"
                     "<b n='{accumulator-before(\"n\")}' total='{accumulator-before(\"total\")}'/>"
                     "</xsl:for-each>",
                     books,
                     DECLARATION "<out><b n=\"1\" total=\"0\"/><b n=\"2\" total=\"3\"/>"
                                 "<b n=\"3\" total=\"8\"/></out>"),
    ACCUMULATOR_CASE("accumulators-of-full-windows", BOOK_ACCUMULATORS, "#all",
                     "<xsl:for-each select='shop/book'><xsl:value-of "
                     "select='copy-of(.)!(accumulator-after(\"n\"), accumulator-after(\"total\"), "
                     "(.//t)!accumulator-before(\"n\"))'/>;</xsl:for-each>",
                     books, DECLARATION "<out>1 3 1;2 8 2;4 15 3 4;</out>"),
    ACCUMULATOR_CASE("accumulators-of-skipped-content", BOOK_ACCUMULATORS, "#all",
                     "<xsl:for-each select='shop/empty'><e n='{accumulator-before(\"n\")}' "
                     "t='{accumulator-before(\"total\")}'/></xsl:for-each>",
                     books, DECLARATION "<out><e n=\"4\" t=\"15\"/></out>"),
    ACCUMULATOR_CASE("accumulators-not-applicable", BOOK_ACCUMULATORS, "n",
                     "<xsl:for-each select='shop/book[1]'><xsl:try><xsl:value-of "
                     "select='accumulator-before(\"total\")'/><xsl:catch "
                     "select='local-name-from-QName($err:code)' "
                     "xmlns:err='http://www.w3.org/2005/xqt-errors'/></xsl:try></xsl:for-each>",
                     books, DECLARATION "<out>XTDE3362</out>"),
    /* 19.5: a rule's error is raised when a value it left out is read. */
    ACCUMULATOR_CASE("accumulator-errors-when-read",
                     "<xsl:accumulator name='e' initial-value='1' streamable='yes'>"
                     "<xsl:accumulator-rule match='book[@id = \"1\"]' select='$value div 0'/>"
                     "</xsl:accumulator>",
                     "#all",
                     "<xsl:for-each select='shop/book'><xsl:try><xsl:value-of "
                     "select='accumulator-before(\"e\")'/><xsl:catch>!</xsl:catch></xsl:try>"
                     "</xsl:for-each>",
                     books, DECLARATION "<out>!!!</out>"),
    /* The values of one node depend on each other, whatever the order of
       the declarations (19.4). */
    ACCUMULATOR_CASE("dependent-accumulators",
                     "<xsl:accumulator name='a' initial-value='0' streamable='yes'>"
                     "<xsl:accumulator-rule match='book' select='accumulator-before(\"b\") * 10'/>"
                     "</xsl:accumulator>"
                     "<xsl:accumulator name='b' initial-value='0' streamable='yes'>"
                     "<xsl:accumulator-rule match='book' select='$value + 1'/></xsl:accumulator>",
                     "#all",
                     "<xsl:for-each select='shop/book'>"
                     "<xsl:value-of select='accumulator-before(\"a\")'/>;</xsl:for-each>",
                     books, DECLARATION "<out>10;20;30;</out>"),
    /* 19.9: end rules with capture="yes" read the node's subtree, captured
       as its events pass (packet 7.6g): skipped elements, nested ones,
       a selected one in its full window, an empty one. */
    ACCUMULATOR_CASE("captured-end-rules",
                     "<xsl:accumulator name='titles' initial-value='()' streamable='yes'>"
                     "<xsl:accumulator-rule match='book' phase='end' capture='yes' "
                     "select='($value, string(t))'/></xsl:accumulator>",
                     "#all",
                     "<xsl:for-each select='shop/empty'>"
                     "<xsl:value-of select='accumulator-before(\"titles\")' separator=','/>"
                     "</xsl:for-each>",
                     books, DECLARATION "<out>A,B,D,C</out>"),
    ACCUMULATOR_CASE("captured-end-rules-of-selected-nodes",
                     "<xsl:accumulator name='titles' initial-value='()' streamable='yes'>"
                     "<xsl:accumulator-rule match='book' phase='end' capture='yes' "
                     "select='($value, string(t))'/></xsl:accumulator>",
                     "#all",
                     "<xsl:for-each select='shop/book'>"
                     "<xsl:value-of select='accumulator-after(\"titles\")' separator=','/>;"
                     "</xsl:for-each>",
                     books, DECLARATION "<out>A;A,B;A,B,D,C;</out>"),
    ACCUMULATOR_CASE("captured-empty-elements",
                     "<xsl:accumulator name='names' initial-value='\"\"' streamable='yes'>"
                     "<xsl:accumulator-rule match='e' phase='end' capture='yes' "
                     "select='$value || name(.) || count(node())'/>"
                     "<xsl:accumulator-rule match='f' phase='start' select='$value || \"+\"'/>"
                     "</xsl:accumulator>",
                     "#all",
                     "<xsl:for-each select='r/f'>"
                     "<xsl:value-of select='accumulator-before(\"names\")'/></xsl:for-each>",
                     "<r><e/><e>x<g/></e><f/></r>", DECLARATION "<out>e0e2+</out>"),
    /* xsl:fork: each prong a pass of its own over the document (D231). */
    STREAM_CASE("fork-prongs-in-passes", "",
                "<xsl:fork><xsl:sequence><a><xsl:value-of select='count(shop/book)'/></a>"
                "</xsl:sequence><xsl:sequence select='sum(shop/book/p)'/>"
                "<xsl:sequence><xsl:for-each select='shop/book'><b id='{@id}'/></xsl:for-each>"
                "</xsl:sequence></xsl:fork>",
                books,
                DECLARATION "<out><a>3</a>15<b id=\"1\"/><b id=\"2\"/><b id=\"3\"/></out>"),
    STREAM_CASE("unused-accumulator",
                "<xsl:accumulator name='a' initial-value='0'>"
                "<xsl:accumulator-rule match='book' select='$value + 1'/></xsl:accumulator>",
                "<xsl:value-of select='count(shop/book)'/>", books, DECLARATION "<out>3</out>"),
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
                const sxml_error_record *error = &test_fixture.error_record;
                printf("  %s (%s): status %d %.*s %.*s, got %.*s\n", test_case->case_name,
                       text_index == 0 ? "streamed" : "unstreamed", (int)status,
                       (int)error->code.local_name.byte_count,
                       (const char *)error->code.local_name.bytes,
                       (int)error->diagnostic.description.byte_count,
                       (const char *)error->diagnostic.description.bytes, (int)got.byte_count,
                       (const char *)got.bytes);
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

/* Two downward selections in one text value template of xsl:text are not
   guaranteed-streamable (XSLT Streaming 4.0, 12.6.40: the content is
   absorbed): XTSE3430, as in xsl:value-of. */
static void test_text_value_templates_are_assessed(sxml_test_state *state) {
    fixture test_fixture;
    sxml_status status = fixture_create(
        &test_fixture, (sxml_byte_view){.bytes = (const uint8_t *)books, .byte_count = sizeof(books) - 1});
    static const char *const not_streamable[] = {
        STREAMED("", "yes", "<xsl:text expand-text='yes'>{count(//book)} {count(//t)}</xsl:text>"),
        STREAMED("", "yes", "<p xsl:expand-text='yes'>{count(//book)} {count(//t)}</p>"),
    };
    for (size_t case_index = 0; status == SXML_STATUS_OK && case_index < SXML_TEST_COUNT(not_streamable);
         case_index += 1) {
        const sxml_status run_status = run_main(
            &test_fixture, (sxml_byte_view){.bytes = (const uint8_t *)not_streamable[case_index],
                                            .byte_count = strlen(not_streamable[case_index])});
        SXML_TEST_EXPECT(state, run_status == SXML_STATUS_STATIC_ERROR);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(test_fixture.error_record.code.local_name,
                                                      TEXT("XTSE3430")));
    }
    static const char streamable[] =
        STREAMED("", "yes", "<xsl:text expand-text='yes'>{count(//book)} books</xsl:text>");
    if (status == SXML_STATUS_OK) {
        status = run_main(&test_fixture, TEXT(streamable));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                TEXT(DECLARATION "<out>4 books</out>")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------- xsl:merge */

/* A stylesheet whose template main merges in.xml and in2.xml (packet
   7.6g): its merge sources, streamable or not, and its action. */
#define MERGED(sources, action)                                                                    \
    "<xsl:stylesheet version='3.0' " XSL_NAMESPACE                                                 \
    " xmlns:xs='http://www.w3.org/2001/XMLSchema' exclude-result-prefixes='#all'>"                 \
    "<xsl:template name='main'><out><xsl:merge>" sources "<xsl:merge-action>" action               \
    "</xsl:merge-action></xsl:merge></out></xsl:template></xsl:stylesheet>"

/* A merge source over the documents for-each-source names. */
#define MERGE_SOURCE(name, documents, streamable, select, key)                                     \
    "<xsl:merge-source name='" name "' for-each-source=\"" documents "\" streamable='" streamable  \
    "' select='" select "'><xsl:merge-key select='" key "'/></xsl:merge-source>"

#define GROUP_ACTION                                                                               \
    "<g k='{current-merge-key()}' p='{position()}'><xsl:copy-of select='current-merge-group()'/>"  \
    "</g>"

typedef struct merge_case {
    const char *case_name;
    const char *streamed_text;
    const char *unstreamed_text;
    const char *document_text;
    const char *second_document_text;
    /* The output, or NULL for the error expected_error names. */
    const char *expected_output;
    const char *expected_error;
} merge_case;

#define TWO_SOURCE_CASE(name, select_a, select_b, key, action, first, second, expected)            \
    {                                                                                              \
        .case_name = (name),                                                                       \
        .streamed_text = MERGED(MERGE_SOURCE("a", "'in.xml'", "yes", select_a, key)                \
                                    MERGE_SOURCE("b", "'in2.xml'", "yes", select_b, key),          \
                                action),                                                           \
        .unstreamed_text = MERGED(MERGE_SOURCE("a", "'in.xml'", "no", select_a, key)               \
                                      MERGE_SOURCE("b", "'in2.xml'", "no", select_b, key),         \
                                  action),                                                         \
        .document_text = (first), .second_document_text = (second), .expected_output = (expected), \
        .expected_error = NULL                                                                     \
    }

static const char merge_first[] = "<r><x k='1'/><x k='3'/><x k='3' n='2'/><x k='5'/></r>";
static const char merge_second[] = "<r><w/><y k='2'/><y k='3'/><y k='4'/></r>";

static const merge_case merge_cases[] = {
    TWO_SOURCE_CASE("two-streams", "r/x", "r/y", "xs:integer(@k)", GROUP_ACTION, merge_first,
                    merge_second,
                    DECLARATION "<out><g k=\"1\" p=\"1\"><x k=\"1\"/></g><g k=\"2\" p=\"2\">"
                                "<y k=\"2\"/></g><g k=\"3\" p=\"3\"><x k=\"3\"/><x k=\"3\" "
                                "n=\"2\"/><y k=\"3\"/></g><g k=\"4\" p=\"4\"><y k=\"4\"/></g>"
                                "<g k=\"5\" p=\"5\"><x k=\"5\"/></g></out>"),
    TWO_SOURCE_CASE("named-groups", "r/x", "/r/*", "xs:integer(@k)",
                    "<g k='{current-merge-key()}' a='{count(current-merge-group(\"a\"))}' "
                    "b='{count(current-merge-group(\"b\"))}'/>",
                    merge_first, merge_second,
                    DECLARATION "<out><g k=\"\" a=\"0\" b=\"1\"/><g k=\"1\" a=\"1\" b=\"0\"/>"
                                "<g k=\"2\" a=\"0\" b=\"1\"/><g k=\"3\" a=\"2\" b=\"1\"/>"
                                "<g k=\"4\" a=\"0\" b=\"1\"/><g k=\"5\" a=\"1\" b=\"0\"/></out>"),
    {.case_name = "descending-anchors",
     .streamed_text = MERGED(MERGE_SOURCE("a", "('in2.xml', 'in.xml')", "yes", "r/*",
                                          "xs:integer(@k)")
                                 "<xsl:merge-source select='()'><xsl:merge-key "
                                 "select='xs:integer(@k)'/></xsl:merge-source>",
                             "<g k='{current-merge-key()}' n='{current-merge-group() ! name()}'/>"),
     .unstreamed_text = MERGED(MERGE_SOURCE("a", "('in2.xml', 'in.xml')", "no", "r/*",
                                            "xs:integer(@k)")
                                   "<xsl:merge-source select='()'><xsl:merge-key "
                                   "select='xs:integer(@k)'/></xsl:merge-source>",
                               "<g k='{current-merge-key()}' n='{current-merge-group() ! name()}'/>"),
     .document_text = "<r><x k='5'/><x k='3'/></r>",
     .second_document_text = "<r><y k='4'/><y k='3'/></r>",
     .expected_output = NULL,
     .expected_error = "XTDE2220"},
    {.case_name = "grounded-and-streamed",
     .streamed_text = MERGED(MERGE_SOURCE("a", "'in.xml'", "yes", "r/x", "xs:integer(@k)")
                                 "<xsl:merge-source name='b' select='doc(\"in2.xml\")/r/y'>"
                                 "<xsl:merge-key select='xs:integer(@k)'/></xsl:merge-source>",
                             GROUP_ACTION),
     .unstreamed_text = MERGED(MERGE_SOURCE("a", "'in.xml'", "no", "r/x", "xs:integer(@k)")
                                   "<xsl:merge-source name='b' select='doc(\"in2.xml\")/r/y'>"
                                   "<xsl:merge-key select='xs:integer(@k)'/></xsl:merge-source>",
                               GROUP_ACTION),
     .document_text = merge_first,
     .second_document_text = merge_second,
     .expected_output =
         DECLARATION "<out><g k=\"1\" p=\"1\"><x k=\"1\"/></g><g k=\"2\" p=\"2\"><y k=\"2\"/></g>"
                     "<g k=\"3\" p=\"3\"><x k=\"3\"/><x k=\"3\" n=\"2\"/><y k=\"3\"/></g>"
                     "<g k=\"4\" p=\"4\"><y k=\"4\"/></g><g k=\"5\" p=\"5\"><x k=\"5\"/></g></out>",
     .expected_error = NULL},
    /* An input out of order is an error once it is read (15.4). */
    {.case_name = "input-out-of-order",
     .streamed_text = MERGED(MERGE_SOURCE("a", "'in.xml'", "yes", "r/x", "xs:integer(@k)"),
                             GROUP_ACTION),
     .unstreamed_text = MERGED(MERGE_SOURCE("a", "'in.xml'", "no", "r/x", "xs:integer(@k)"),
                               GROUP_ACTION),
     .document_text = "<r><x k='1'/><x k='3'/><x k='2'/></r>",
     .second_document_text = merge_second,
     .expected_output = NULL,
     .expected_error = "XTDE2220"},
    TWO_SOURCE_CASE("descending-keys", "r/x", "r/y", "xs:integer(@k)' order='descending",
                    "<xsl:value-of select='current-merge-key()'/>,",
                    "<r><x k='5'/><x k='1'/></r>", "<r><y k='4'/><y k='2'/></r>",
                    DECLARATION "<out>5,4,2,1,</out>"),
};

/* Runs text over the case's documents into the output; the status. */
static sxml_status run_merge(fixture *test_fixture, const char *text) {
    return run_main(test_fixture,
                    (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = strlen(text)});
}

/* Whether the run ended as the case expects: its output, or its error. */
static bool merge_ended_as_expected(const fixture *test_fixture, const merge_case *test_case,
                                    sxml_status status) {
    if (test_case->expected_error != NULL) {
        return status != SXML_STATUS_OK &&
               sxml_byte_view_equals(test_fixture->error_record.code.local_name,
                                     sxml_byte_view_from_c_string(test_case->expected_error));
    }
    return status == SXML_STATUS_OK &&
           sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                 sxml_byte_view_from_c_string(test_case->expected_output));
}

/* Streamed merge sources (MERGE, packet 7.6g) give what the grounded
   merge gives (19.10): groups in key order, their items in source,
   anchor and input order, named groups, a grounded source among
   streamed ones, descending keys, and XTDE2220. */
static void test_streamed_merges_are_unstreamed_merges(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(merge_cases); case_index += 1) {
        const merge_case *test_case = &merge_cases[case_index];
        fixture test_fixture;
        sxml_status status =
            fixture_create(&test_fixture, sxml_byte_view_from_c_string(test_case->document_text));
        test_fixture.served.second_content =
            sxml_byte_view_from_c_string(test_case->second_document_text);
        const char *texts[2] = {test_case->streamed_text, test_case->unstreamed_text};
        for (size_t text_index = 0; status == SXML_STATUS_OK && text_index < 2; text_index += 1) {
            const sxml_status run = run_merge(&test_fixture, texts[text_index]);
            const bool is_expected = merge_ended_as_expected(&test_fixture, test_case, run);
            if (!is_expected) {
                const sxml_byte_view got = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
                printf("  %s (%s): status %d %.*s %.*s, got %.*s\n", test_case->case_name,
                       text_index == 0 ? "streamed" : "unstreamed", (int)run,
                       (int)test_fixture.error_record.code.local_name.byte_count,
                       (const char *)test_fixture.error_record.code.local_name.bytes,
                       (int)test_fixture.error_record.diagnostic.description.byte_count,
                       (const char *)test_fixture.error_record.diagnostic.description.bytes,
                       (int)got.byte_count, (const char *)got.bytes);
            }
            SXML_TEST_EXPECT(state, is_expected);
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
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

/* Two generated documents of LARGE_RECORD_COUNT records each, ids even
   and odd, merged by id (packet 7.6g): streamed, the cursors hold a
   window each, under the limit the grounded merge exceeds. */
typedef struct records_request {
    uint32_t first_id;
    uint32_t id_step;
} records_request;

static sxml_status make_records_document(records_request request, sxml_byte_view *document,
                                         void **memory) {
    const size_t capacity = (size_t)LARGE_RECORD_COUNT * 64U + 64U;
    sxml_status status = sxml_allocator_allocate(
        sxml_default_allocator(), (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
        memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    char *text = *memory;
    size_t length = 0;
    length += (size_t)snprintf(text + length, capacity - length, "<records>\n");
    for (uint32_t record = 0; record < LARGE_RECORD_COUNT; record += 1) {
        length += (size_t)snprintf(text + length, capacity - length,
                                   "  <record id='%u'><v>%u</v></record>\n",
                                   request.first_id + record * request.id_step, record % 7U);
    }
    length += (size_t)snprintf(text + length, capacity - length, "</records>\n");
    *document = (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = length};
    return SXML_STATUS_OK;
}

#define LARGE_MERGE(streamable)                                                                    \
    MERGED(MERGE_SOURCE("a", "'in.xml'", streamable, "records/record", "xs:integer(@id)")          \
               MERGE_SOURCE("b", "'in2.xml'", streamable, "records/record", "xs:integer(@id)"),    \
           "<xsl:if test='current-merge-key() mod 20000 = 0'><r k='{current-merge-key()}' "        \
           "v='{current-merge-group()/v}'/></xsl:if>")

static void test_large_merge_streams_in_bounded_memory(sxml_test_state *state) {
    sxml_byte_view documents[2] = {{.bytes = NULL, .byte_count = 0},
                                   {.bytes = NULL, .byte_count = 0}};
    void *memory[2] = {NULL, NULL};
    sxml_status status = make_records_document(
        (records_request){.first_id = 2, .id_step = 2}, &documents[0], &memory[0]);
    if (status == SXML_STATUS_OK) {
        status = make_records_document((records_request){.first_id = 1, .id_step = 2},
                                       &documents[1], &memory[1]);
    }
    fixture test_fixture;
    if (status == SXML_STATUS_OK) {
        status = fixture_create(&test_fixture, documents[0]);
        test_fixture.served.second_content = documents[1];
        test_fixture.memory.limit_byte_count = STREAM_LIMIT_BYTE_COUNT;
    }
    if (status == SXML_STATUS_OK) {
        status = run_main(&test_fixture, TEXT(LARGE_MERGE("yes")));
        if (status != SXML_STATUS_OK) {
            printf("  streamed merge: status %d, %.*s: %.*s\n", (int)status,
                   (int)test_fixture.error_record.code.local_name.byte_count,
                   (const char *)test_fixture.error_record.code.local_name.bytes,
                   (int)test_fixture.error_record.diagnostic.description.byte_count,
                   (const char *)test_fixture.error_record.diagnostic.description.bytes);
        }
        printf("  documents %zu and %zu bytes; streamed merge peak %zu bytes under a limit of "
               "%zu\n",
               documents[0].byte_count, documents[1].byte_count,
               test_fixture.memory.peak_byte_count, (size_t)STREAM_LIMIT_BYTE_COUNT);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                    TEXT(DECLARATION "<out><r k=\"20000\" v=\"3\"/>"
                                                     "<r k=\"40000\" v=\"0\"/>"
                                                     "<r k=\"60000\" v=\"4\"/></out>")));
        sxml_status unstreamed = SXML_STATUS_OK;
        if (status == SXML_STATUS_OK) {
            unstreamed = run_main(&test_fixture, TEXT(LARGE_MERGE("no")));
        }
        SXML_TEST_EXPECT(state, unstreamed != SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    for (size_t document_index = 0; document_index < 2; document_index += 1) {
        sxml_allocator_release(sxml_default_allocator(), memory[document_index]);
    }
}

/* Streamed paths over the large document: counted, atomized and
   attribute values stream under the limit (D223), which the same
   expressions unstreamed exceed. */
typedef struct large_path_case {
    const char *streamed_text;
    const char *unstreamed_text;
    const char *expected_output;
} large_path_case;

#define LARGE_PATH_CASE(declarations, body, expected)                                              \
    {                                                                                              \
        .streamed_text = STREAMED(declarations, "yes", body),                                      \
        .unstreamed_text = STREAMED(declarations, "no", body),                                     \
        .expected_output = DECLARATION "<out>" expected "</out>"                                    \
    }

static const large_path_case large_path_cases[] = {
    LARGE_PATH_CASE("", "<xsl:value-of select='count(records/record)'/>", "30000"),
    LARGE_PATH_CASE("", "<xsl:value-of select='sum(records/record/price)'/>", "1.4925E6"),
    LARGE_PATH_CASE("", "<xsl:value-of select='sum(//record/@id) div 1000'/>", "450015"),
    LARGE_PATH_CASE("", "<xsl:value-of select='max(records/record/@id)'/>", "30000"),
    /* Two consuming prongs of an xsl:fork, a pass each (D231). */
    LARGE_PATH_CASE("",
                    "<xsl:fork><xsl:sequence select='count(records/record)'/>"
                    "<xsl:sequence select='sum(records/record/price)'/></xsl:fork>",
                    "30000 1.4925E6"),
    /* avg folded as sum and count (D228). */
    LARGE_PATH_CASE("", "<xsl:value-of select='avg(records/record/price)'/>", "49.75"),
    LARGE_PATH_CASE("", "<c><xsl:copy-of select='records/record[@hot = \"yes\"]/name'/></c>",
                    "<c><name>item 10000</name><name>item 20000</name><name>item 30000</name></c>"),
    LARGE_PATH_CASE("<xsl:mode name='s' streamable='yes' on-no-match='shallow-skip'/>"
                    "<xsl:template match='record[@hot = \"yes\"]' mode='s'>"
                    "<r n='{position()}'><xsl:value-of select='name'/></r></xsl:template>",
                    "<xsl:apply-templates select='records/record' mode='s'/>",
                    "<r n=\"10000\">item 10000</r><r n=\"20000\">item 20000</r>"
                    "<r n=\"30000\">item 30000</r>"),
};

static void test_large_document_paths_stream_in_bounded_memory(sxml_test_state *state) {
    sxml_byte_view document = {.bytes = NULL, .byte_count = 0};
    void *memory = NULL;
    sxml_status status = make_large_document(&document, &memory);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        return;
    }
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(large_path_cases); case_index += 1) {
        const large_path_case *test_case = &large_path_cases[case_index];
        fixture test_fixture;
        status = fixture_create(&test_fixture, document);
        test_fixture.memory.limit_byte_count = STREAM_LIMIT_BYTE_COUNT;
        if (status == SXML_STATUS_OK) {
            status = run_main(&test_fixture,
                              (sxml_byte_view){.bytes = (const uint8_t *)test_case->streamed_text,
                                               .byte_count = strlen(test_case->streamed_text)});
        }
        const sxml_byte_view got = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
        printf("  %s: status %d, peak %zu bytes, %.*s\n", test_case->streamed_text, (int)status,
               test_fixture.memory.peak_byte_count, (int)got.byte_count, (const char *)got.bytes);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(
            state, sxml_byte_view_equals(
                       got, (sxml_byte_view){.bytes = (const uint8_t *)test_case->expected_output,
                                             .byte_count = strlen(test_case->expected_output)}));
        sxml_status unstreamed = SXML_STATUS_OK;
        if (status == SXML_STATUS_OK) {
            unstreamed =
                run_main(&test_fixture,
                         (sxml_byte_view){.bytes = (const uint8_t *)test_case->unstreamed_text,
                                          .byte_count = strlen(test_case->unstreamed_text)});
        }
        SXML_TEST_EXPECT(state, unstreamed != SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    sxml_allocator_release(sxml_default_allocator(), memory);
}

/* -------------------------------------------- Streamed principal source */

/* A stylesheet whose unnamed mode is streamable or not, with templates. */
#define PRINCIPAL(streamable, on_no_match, templates)                                              \
    "<xsl:stylesheet version='3.0' " XSL_NAMESPACE " exclude-result-prefixes='#all'>"              \
    "<xsl:mode streamable='" streamable "' on-no-match='" on_no_match "'/>" templates              \
    "</xsl:stylesheet>"

/* As PRINCIPAL, with every accumulator applicable (19.2). */
#define PRINCIPAL_ACCUMULATED(streamable, on_no_match, templates)                                  \
    "<xsl:stylesheet version='3.0' " XSL_NAMESPACE " exclude-result-prefixes='#all'>"              \
    "<xsl:mode streamable='" streamable "' on-no-match='" on_no_match                              \
    "' use-accumulators='#all'/>" templates "</xsl:stylesheet>"

/* Counts the records (D231). */
#define RECORD_COUNTER                                                                             \
    "<xsl:accumulator name='n' initial-value='0' streamable='yes'>"                                \
    "<xsl:accumulator-rule match='record' select='$value + 1'/></xsl:accumulator>"

/* Applies the templates of stylesheet_text's default mode to the streamed
   principal source in.xml (D228), delivered as result_delivery says: the
   serialization into the output buffer, or the tree into *principal. */
typedef struct principal_run {
    sxml_byte_view stylesheet_text;
    sxml_xslt_result_delivery result_delivery;
    sxml_sequence *principal;
} principal_run;

static sxml_status run_principal_delivered(fixture *test_fixture, const principal_run *run) {
    const sxml_byte_view stylesheet_text = run->stylesheet_text;
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
            .error_record = &test_fixture->error_record},
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
                .streamed_source_uri = TEXT("http://example.com/x/in.xml"),
                .base_output_uri = TEXT("http://example.com/out/main.xml"),
                .output_definition = NULL,
                .output_stream = sxml_output_buffer_as_output_stream(test_fixture->output_buffer),
                .error_record = &test_fixture->error_record,
                .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,
                .result_delivery = run->result_delivery},
            &principal);
        if (run->principal != NULL) {
            *run->principal = principal;
        } else {
            sxml_sequence_release(principal);
        }
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

/* Serialized into the output buffer. */
static sxml_status run_principal(fixture *test_fixture, sxml_byte_view stylesheet_text) {
    return run_principal_delivered(
        test_fixture, &(principal_run){.stylesheet_text = stylesheet_text,
                                       .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED,
                                       .principal = NULL});
}

/* Template rules applied to a streamed principal source (D228): the
   streamed mode, and the same templates in a mode that is not
   streamable, which grounds the whole document first. */
typedef struct principal_case {
    const char *streamed_text;
    const char *unstreamed_text;
    /* The output; NULL: the same as the unstreamed run's. */
    const char *expected_output;
} principal_case;

#define PRINCIPAL_CASE(on_no_match, templates, expected)                                           \
    {                                                                                              \
        .streamed_text = PRINCIPAL("yes", on_no_match, templates),                                 \
        .unstreamed_text = PRINCIPAL("no", on_no_match, templates), .expected_output = (expected)  \
    }

static const principal_case principal_cases[] = {
    /* A template rule for "/" with a stream point of its own (glossary
       `streamed_template_rule`). */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><out><xsl:for-each "
                   "select='records/record[@hot = \"yes\"]'><r id='{@id}'><xsl:value-of "
                   "select='name'/></r></xsl:for-each></out></xsl:template>",
                   DECLARATION "<out><r id=\"10000\">item 10000</r><r id=\"20000\">item 20000</r>"
                               "<r id=\"30000\">item 30000</r></out>"),
    /* The built-in rule applies templates to the children as they pass;
       a rule that reads its node's content gets its full window. */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><out><xsl:apply-templates/></out></xsl:template>"
                   "<xsl:template match='record[@hot = \"yes\"]'><xsl:copy-of select='.'/>"
                   "</xsl:template>",
                   NULL),
    /* The identity, as the document streams: the shallow copies' content
       is delivered as it is made. */
    PRINCIPAL_CASE("shallow-copy",
                   "<xsl:template match='record[@hot = \"no\"]'/>"
                   "<xsl:template match='price'><p><xsl:value-of select='. * 2'/></p>"
                   "</xsl:template>",
                   NULL),
    /* Accumulators computed as the document streams (D231): before each
       hot record, after the document node. */
    {.streamed_text = PRINCIPAL_ACCUMULATED(
         "yes", "shallow-skip",
         RECORD_COUNTER "<xsl:template match='/'><out><xsl:apply-templates/><total "
                        "n='{accumulator-after(\"n\")}'/></out></xsl:template>"
                        "<xsl:template match='record[@hot = \"yes\"]'><r "
                        "n='{accumulator-before(\"n\")}'/></xsl:template>"),
     .unstreamed_text = PRINCIPAL_ACCUMULATED(
         "no", "shallow-skip",
         RECORD_COUNTER "<xsl:template match='/'><out><xsl:apply-templates/><total "
                        "n='{accumulator-after(\"n\")}'/></out></xsl:template>"
                        "<xsl:template match='record[@hot = \"yes\"]'><r "
                        "n='{accumulator-before(\"n\")}'/></xsl:template>"),
     .expected_output = DECLARATION "<out><r n=\"10000\"/><r n=\"20000\"/><r n=\"30000\"/>"
                                    "<total n=\"30000\"/></out>"},
    /* The whole identity, serialized as it is made (D231): the principal
       result is not held, so its size is not in the limit. */
    PRINCIPAL_CASE("shallow-copy", "", NULL),
    /* The same under a wrapper the rule for "/" makes. */
    PRINCIPAL_CASE("shallow-copy",
                   "<xsl:template match='/'><out><xsl:apply-templates/></out></xsl:template>"
                   "<xsl:template match='name'><n><xsl:value-of select='.'/></n></xsl:template>",
                   NULL),
    /* Text after text, more than the limit (D295): with the text
       method each text event is written as it comes; with the xml method
       one text node of 1.6 MB under one element is written in parts. Both
       built the principal result's tree before, and its text run was
       copied at each event (quadratic). */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:output method='text'/>"
                   "<xsl:template match='/'><xsl:for-each select='records/record'>"
                   "<xsl:value-of select='name'/><xsl:text> is a record of the document&#10;"
                   "</xsl:text></xsl:for-each></xsl:template>",
                   NULL),
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><out><xsl:for-each select='records/record'>"
                   "<xsl:value-of select='name'/><xsl:text> is a record of the document&#10;"
                   "</xsl:text></xsl:for-each></out></xsl:template>",
                   NULL),
    /* An aggregate over the document from the rule for "/". */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><n><xsl:value-of "
                   "select='sum(records/record/price)'/></n></xsl:template>",
                   DECLARATION "<n>1.4925E6</n>"),
    /* Text value templates are absorbed (XSLT Streaming 4.0, 12.5,
       12.6.40), in xsl:text as in a text node: streamed paths. xsl:text
       was taken as grounded and read a shallow window: 0 records. */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/' expand-text='yes'><n><xsl:text>{count(records/record)}"
                   "</xsl:text></n></xsl:template>",
                   DECLARATION "<n>30000</n>"),
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/' expand-text='yes'><s>{sum(records/record/price)}</s>"
                   "</xsl:template>",
                   DECLARATION "<s>1.4925E6</s>"),
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/' expand-text='yes'><n>hot: "
                   "<xsl:text>{records/record[@hot = 'yes']/@id}</xsl:text></n></xsl:template>",
                   DECLARATION "<n>hot: 10000 20000 30000</n>"),
    /* xsl:iterate over the records as they pass (D302, glossary
       `streamed_iteration`): it was a streamed path, all records
       grounded before the first iteration. */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><n><xsl:iterate select='records/record'>"
                   "<xsl:param name='n' select='0'/><xsl:on-completion><c><xsl:value-of "
                   "select='$n'/></c></xsl:on-completion><xsl:if test='@hot = \"yes\"'><h>"
                   "<xsl:value-of select='name'/></h></xsl:if><xsl:next-iteration>"
                   "<xsl:with-param name='n' select='$n + 1'/></xsl:next-iteration>"
                   "</xsl:iterate></n></xsl:template>",
                   DECLARATION "<n><h>item 10000</h><h>item 20000</h><h>item 30000</h>"
                               "<c>30000</c></n>"),
    /* xsl:break ends the reading: no iteration after it. */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><n><xsl:iterate select='records/record'>"
                   "<xsl:param name='hot' select='0'/><xsl:on-completion>none</xsl:on-completion>"
                   "<xsl:choose><xsl:when test='@hot = \"yes\" and $hot = 1'><xsl:break "
                   "select='string(@id)'/></xsl:when><xsl:otherwise><xsl:next-iteration>"
                   "<xsl:with-param name='hot' select='$hot + (if (@hot = \"yes\") then 1 "
                   "else 0)'/></xsl:next-iteration></xsl:otherwise></xsl:choose>"
                   "</xsl:iterate></n></xsl:template>",
                   DECLARATION "<n>20000</n>"),
    /* Every record an output, delivered as it is made: the outputs are a
       content LET (D166), not held. */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><n><xsl:iterate select='records/record'>"
                   "<r><xsl:value-of select='@id'/></r></xsl:iterate></n></xsl:template>",
                   NULL),
    /* xsl:for-each-group whose groups form as the records pass (D302):
       adjacent keys, and groups starting with a pattern. A group is held
       while it forms: these have 10 and 100 records. */
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><n><xsl:for-each-group select='records/record' "
                   "group-adjacent='@id idiv 10'><g k='{current-grouping-key()}' "
                   "p='{position()}'><xsl:value-of select='sum(current-group()/price)'/></g>"
                   "</xsl:for-each-group></n></xsl:template>",
                   NULL),
    PRINCIPAL_CASE("shallow-skip",
                   "<xsl:template match='/'><n><xsl:for-each-group select='records/record' "
                   "group-starting-with='record[@id mod 100 = 0]'><g id='{@id}'>"
                   "<xsl:value-of select='count(current-group())'/></g>"
                   "</xsl:for-each-group></n></xsl:template>",
                   NULL),
};

static void test_principal_source_streams_in_bounded_memory(sxml_test_state *state) {
    sxml_byte_view document = {.bytes = NULL, .byte_count = 0};
    void *memory = NULL;
    sxml_status status = make_large_document(&document, &memory);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        return;
    }
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(principal_cases); case_index += 1) {
        const principal_case *test_case = &principal_cases[case_index];
        const sxml_byte_view streamed = {.bytes = (const uint8_t *)test_case->streamed_text,
                                         .byte_count = strlen(test_case->streamed_text)};
        const sxml_byte_view unstreamed = {.bytes = (const uint8_t *)test_case->unstreamed_text,
                                           .byte_count = strlen(test_case->unstreamed_text)};
        fixture test_fixture;
        status = fixture_create(&test_fixture, document);
        /* Unstreamed and unlimited first: the output to compare with. */
        sxml_output_buffer *expected = NULL;
        if (status == SXML_STATUS_OK) {
            status = run_principal(&test_fixture, unstreamed);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_output_buffer_create(
                sxml_default_allocator(),
                &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                    .maximum_byte_count = 0},
                &expected);
        }
        if (status == SXML_STATUS_OK) {
            const sxml_byte_view bytes = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
            const sxml_output_stream stream = sxml_output_buffer_as_output_stream(expected);
            status = stream.operations->write(stream.state, bytes);
        }
        test_fixture.memory.peak_byte_count = test_fixture.memory.live_byte_count;
        test_fixture.memory.limit_byte_count =
            test_fixture.memory.live_byte_count + STREAM_LIMIT_BYTE_COUNT;
        if (status == SXML_STATUS_OK) {
            status = run_principal(&test_fixture, streamed);
        }
        const sxml_byte_view got = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
        printf("  case %zu: status %d, peak %zu bytes, %zu output bytes\n", case_index,
               (int)status, test_fixture.memory.peak_byte_count, got.byte_count);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        if (expected != NULL) {
            SXML_TEST_EXPECT(state,
                             sxml_byte_view_equals(got, sxml_output_buffer_get_bytes(expected)));
        }
        if (test_case->expected_output != NULL) {
            SXML_TEST_EXPECT(
                state,
                sxml_byte_view_equals(
                    got, (sxml_byte_view){.bytes = (const uint8_t *)test_case->expected_output,
                                          .byte_count = strlen(test_case->expected_output)}));
        }
        /* Under the same limit the unstreamed run builds the document. */
        sxml_status limited = SXML_STATUS_OK;
        if (status == SXML_STATUS_OK) {
            limited = run_principal(&test_fixture, unstreamed);
        }
        SXML_TEST_EXPECT(state, limited != SXML_STATUS_OK);
        sxml_output_buffer_destroy(expected);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    sxml_allocator_release(sxml_default_allocator(), memory);
}

/* The html and xhtml methods serialized as the principal result is made
   (D297): byte for byte what serializing its tree gives, with the output
   parameters the case names (xsl:output's, indent resolved), in memory far
   below the tree's. */
typedef struct html_principal_case {
    const char *stylesheet_text;
    sxml_serialization_parameters parameters;
} html_principal_case;

#define HTML_PRINCIPAL(output, templates)                                                          \
    "<xsl:stylesheet version='3.0' " XSL_NAMESPACE " exclude-result-prefixes='#all'>" output       \
    "<xsl:mode streamable='yes' on-no-match='shallow-skip'/>" templates "</xsl:stylesheet>"

/* A table row a record, with a script element whose text is made of
   several text events. */
#define HTML_TABLE                                                                                 \
    "<xsl:template match='/'><html><head><title>records</title></head><body><table>"               \
    "<xsl:for-each select='records/record'><tr id='r{@id}'><td><script>if (n &lt; "                \
    "<xsl:value-of select='@id'/> &amp;&amp; m) f('<xsl:value-of select='name'/>');"               \
    "</script></td></tr></xsl:for-each></table></body></html></xsl:template>"

static const html_principal_case html_principal_cases[] = {
    {.stylesheet_text = HTML_PRINCIPAL("<xsl:output method='html' indent='no'/>", HTML_TABLE),
     .parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_HTML,
                    .indent = SXML_SERIALIZATION_FLAG_NO}},
    /* The method chosen from the html element (26.1.2). */
    {.stylesheet_text = HTML_PRINCIPAL("<xsl:output indent='no' html-version='4.01'/>", HTML_TABLE),
     .parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_HTML,
                    .indent = SXML_SERIALIZATION_FLAG_NO,
                    .has_html_version = true,
                    .html_version = SXML_BYTE_VIEW_INITIALIZER("4.01")}},
    {.stylesheet_text = HTML_PRINCIPAL(
         "<xsl:output method='xhtml' indent='no' omit-xml-declaration='no'/>",
         "<xsl:template match='/'><html xmlns='http://www.w3.org/1999/xhtml'><head/><body>"
         "<xsl:for-each select='records/record'><p><xsl:value-of select='name'/><br/>"
         "</p></xsl:for-each></body></html></xsl:template>"),
     .parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_XHTML,
                    .indent = SXML_SERIALIZATION_FLAG_NO,
                    .omit_xml_declaration = SXML_SERIALIZATION_FLAG_NO}},
};

/* Runs test_case on document, read whole or chunk_byte_count bytes at a
   time; with a limit, the streamed run is under it. */
typedef struct html_principal_run {
    const html_principal_case *test_case;
    size_t chunk_byte_count;
    bool is_limited;
} html_principal_run;

static void check_html_principal(sxml_test_state *state, sxml_byte_view document,
                                 const html_principal_run *run) {
    const html_principal_case *test_case = run->test_case;
    const sxml_byte_view stylesheet_text = {.bytes = (const uint8_t *)test_case->stylesheet_text,
                                            .byte_count = strlen(test_case->stylesheet_text)};
    fixture test_fixture;
    sxml_status status = fixture_create(&test_fixture, document);
    test_fixture.served.chunk_byte_count = run->chunk_byte_count;
    /* The tree first, unlimited, serialized as the principal result's
       tree would be. */
    sxml_sequence tree = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = run_principal_delivered(
            &test_fixture, &(principal_run){.stylesheet_text = stylesheet_text,
                                            .result_delivery = SXML_XSLT_RESULT_DELIVERY_TREE,
                                            .principal = &tree});
    }
    sxml_output_buffer *expected = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &expected);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(test_fixture.processor),
            &(sxml_serialization_request){.value = tree,
                                          .serialization_parameters = test_case->parameters,
                                          .error_record = &test_fixture.error_record},
            sxml_output_buffer_as_output_stream(expected));
    }
    sxml_sequence_release(tree);
    if (run->is_limited) {
        test_fixture.memory.peak_byte_count = test_fixture.memory.live_byte_count;
        test_fixture.memory.limit_byte_count =
            test_fixture.memory.live_byte_count + STREAM_LIMIT_BYTE_COUNT;
    }
    if (status == SXML_STATUS_OK) {
        status = run_principal(&test_fixture, stylesheet_text);
    }
    const sxml_byte_view got = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
    printf("  chunks of %zu: status %d, peak %zu bytes, %zu output bytes\n",
           run->chunk_byte_count, (int)status, test_fixture.memory.peak_byte_count,
           got.byte_count);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, expected != NULL &&
                                sxml_byte_view_equals(got, sxml_output_buffer_get_bytes(expected)));
    sxml_output_buffer_destroy(expected);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Records whose names have characters of two, three and four bytes and
   markup characters, read three bytes at a time: chunk borders fall
   inside characters and inside the script elements' text. */
static const char html_small_document[] =
    "<records><record id='1'><name>\xC3\xA9t\xC3\xA9 &lt;&amp;&gt; \xE2\x82\xAC</name></record>"
    "<record id='2'><name>\xF0\x9F\x98\x80 '\xE6\x97\xA5\xE6\x9C\xAC'</name></record>"
    "<record id='3'><name>a\xC2\xA0" "b</name></record></records>";

static void test_html_principal_result_streams(sxml_test_state *state) {
    sxml_byte_view large = {.bytes = NULL, .byte_count = 0};
    void *memory = NULL;
    const sxml_status status = make_large_document(&large, &memory);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        return;
    }
    const sxml_byte_view small = {.bytes = (const uint8_t *)html_small_document,
                                  .byte_count = sizeof(html_small_document) - 1};
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(html_principal_cases);
         case_index += 1) {
        const html_principal_case *test_case = &html_principal_cases[case_index];
        check_html_principal(state, large,
                             &(html_principal_run){.test_case = test_case,
                                                   .chunk_byte_count = 0,
                                                   .is_limited = true});
        check_html_principal(state, small,
                             &(html_principal_run){.test_case = test_case,
                                                   .chunk_byte_count = 3,
                                                   .is_limited = false});
        check_html_principal(state, large,
                             &(html_principal_run){.test_case = test_case,
                                                   .chunk_byte_count = 1000,
                                                   .is_limited = true});
    }
    sxml_allocator_release(sxml_default_allocator(), memory);
}

/* The steps a streamed document grants (D295): the identity of the
   30,000 records takes more steps than a bound of 10^6, which holds the
   tree's identity, but each element read grants its steps; a loop that
   reads nothing stops at the bound. */
#define GRANT_STEP_LIMIT 1000000U

static void test_streamed_elements_grant_steps(sxml_test_state *state) {
    sxml_byte_view document = {.bytes = NULL, .byte_count = 0};
    void *memory = NULL;
    sxml_status status = make_large_document(&document, &memory);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        return;
    }
    fixture test_fixture;
    status = fixture_create_with_step_limit(&test_fixture, document, GRANT_STEP_LIMIT);
    if (status == SXML_STATUS_OK) {
        status = run_principal(&test_fixture, TEXT(PRINCIPAL("yes", "shallow-copy", "")));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_output_buffer_get_bytes(test_fixture.output_buffer).byte_count >
                                document.byte_count);
    sxml_status unstreamed = SXML_STATUS_OK;
    if (status == SXML_STATUS_OK) {
        unstreamed = run_principal(&test_fixture, TEXT(PRINCIPAL("no", "shallow-copy", "")));
    }
    SXML_TEST_EXPECT(state, unstreamed == SXML_STATUS_LIMIT_EXCEEDED);
    sxml_status looping = SXML_STATUS_OK;
    if (status == SXML_STATUS_OK) {
        looping = run_principal(
            &test_fixture,
            TEXT(PRINCIPAL("yes", "shallow-skip",
                           "<xsl:template match='record[@id = \"2\"]'><xsl:value-of "
                           "select='some $n in 1 to 100000000000 satisfies $n = 0'/>"
                           "</xsl:template>")));
    }
    printf("  identity streamed: status %d; unstreamed %d; a loop after two records %d\n",
           (int)status, (int)unstreamed, (int)looping);
    SXML_TEST_EXPECT(state, looping == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    sxml_allocator_release(sxml_default_allocator(), memory);
}

/* The library tells whether a request's initial mode streams (D228). */
static void test_streams_source_reads_the_initial_mode(sxml_test_state *state) {
    fixture test_fixture;
    sxml_status status = fixture_create(&test_fixture, TEXT(books));
    /* Streamable; not streamable; streamable, but a call-template
       invocation. */
    const char *const texts[] = {PRINCIPAL("yes", "shallow-skip", ""),
                                 PRINCIPAL("no", "shallow-skip", ""),
                                 PRINCIPAL("yes", "shallow-skip", "")};
    const bool expected[] = {true, false, false};
    for (size_t text_index = 0; status == SXML_STATUS_OK && text_index < SXML_TEST_COUNT(texts);
         text_index += 1) {
        sxml_xslt_stylesheet *stylesheet = NULL;
        status = sxml_xslt_stylesheet_create(
            fixture_allocator(&test_fixture),
            &(sxml_xslt_stylesheet_configuration){
                .processor = test_fixture.processor,
                .principal_module_text = {.bytes = (const uint8_t *)texts[text_index],
                                          .byte_count = strlen(texts[text_index])},
                .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
                .error_record = &test_fixture.error_record},
            &stylesheet);
        bool streams = !expected[text_index];
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_stylesheet_streams_source(
                stylesheet,
                &(sxml_xslt_transformation_request){
                    .invocation_kind = text_index == 2U
                                           ? SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE
                                           : SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES},
                &streams);
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && streams == expected[text_index]);
        sxml_xslt_stylesheet_destroy(stylesheet);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static const sxml_test_case evaluator_stream_test_cases[] = {
    {.name = "streamed_results_are_unstreamed_results",
     .function = test_streamed_results_are_unstreamed_results},
    {.name = "document_node_is_read_as_a_tree", .function = test_document_node_is_read_as_a_tree},
    {.name = "malformed_document_is_an_error", .function = test_malformed_document_is_an_error},
    {.name = "fallback_option_processes_unstreamed",
     .function = test_fallback_option_processes_unstreamed},
    {.name = "text_value_templates_are_assessed",
     .function = test_text_value_templates_are_assessed},
    {.name = "streamed_merges_are_unstreamed_merges",
     .function = test_streamed_merges_are_unstreamed_merges},
    {.name = "large_merge_streams_in_bounded_memory",
     .function = test_large_merge_streams_in_bounded_memory},
    {.name = "large_document_streams_in_bounded_memory",
     .function = test_large_document_streams_in_bounded_memory},
    {.name = "large_document_paths_stream_in_bounded_memory",
     .function = test_large_document_paths_stream_in_bounded_memory},
    {.name = "principal_source_streams_in_bounded_memory",
     .function = test_principal_source_streams_in_bounded_memory},
    {.name = "html_principal_result_streams", .function = test_html_principal_result_streams},
    {.name = "streamed_elements_grant_steps", .function = test_streamed_elements_grant_steps},
    {.name = "streams_source_reads_the_initial_mode",
     .function = test_streams_source_reads_the_initial_mode},
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
