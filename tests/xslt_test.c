/*
 * Tests for include/sxml/xslt.h through the public API alone: stylesheets
 * in the manner of the XSLT 4.0 test sets compiled and run with the three
 * invocation kinds, serialized by their output definitions; stylesheet,
 * static, template and tunnel parameters, initial functions and initial
 * match selections; the deliveries and the result document collector;
 * messages; modules and packages from the resolver; the lifecycle's
 * argument checks; and an allocation-failure loop (D23). Every case checks
 * for leaked cells and allocations.
 *
 * Golden file: tests/xquery_test.c.
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
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* A stylesheet element around declarations. */
#define STYLESHEET(declarations)                                                          \
    "<xsl:stylesheet version='3.0' " XSL_NAMESPACE                                       \
    " xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns:f='urn:f'"                       \
    " exclude-result-prefixes='#all'>" declarations "</xsl:stylesheet>"

/* ---------------------------------------------------------------- Fixture */

static const char included_text[] =
    STYLESHEET("<xsl:template name='included'><inc/></xsl:template>");

/* Raises its errors on line 2 of failing.xsl. */
static const char failing_text[] =
    STYLESHEET("\n<xsl:template name='fails'><xsl:sequence select=\"error(QName('urn:e', 'e:E'))\"/>"
               "</xsl:template><xsl:function name='f:fails'><xsl:sequence "
               "select=\"error(QName('urn:e', 'e:F'))\"/></xsl:function>");

static const char imported_text[] =
    STYLESHEET("<xsl:template match='b'><imported/></xsl:template>"
               "<xsl:template match='c'><imported-c/></xsl:template>");

static const char resolved_text[] =
    STYLESHEET("<xsl:template name='xsl:initial-template'><resolved/></xsl:template>");

static const char package_text[] =
    "<xsl:package name='urn:p' package-version='1.0' version='3.0' " XSL_NAMESPACE ">"
    "<xsl:template name='xsl:initial-template' visibility='public'><package/></xsl:template>"
    "</xsl:package>";

static const char document_text[] = "<doc><v>1</v><v>2</v></doc>";

/* A simplified stylesheet module: no package, not even an implicit one
   (XSLT 4.0, 3.4, 3.7). */
static const char simplified_text[] = "<out xsl:version='3.0' " XSL_NAMESPACE "/>";

static const sxml_resource_entry fixture_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/x/included.xsl"),
     .content = {.bytes = (const uint8_t *)included_text, .byte_count = sizeof(included_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/x/failing.xsl"),
     .content = {.bytes = (const uint8_t *)failing_text, .byte_count = sizeof(failing_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/x/imported.xsl"),
     .content = {.bytes = (const uint8_t *)imported_text, .byte_count = sizeof(imported_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/x/resolved.xsl"),
     .content = {.bytes = (const uint8_t *)resolved_text, .byte_count = sizeof(resolved_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:p"),
     .content = {.bytes = (const uint8_t *)package_text, .byte_count = sizeof(package_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:not-a-package"),
     .content = {.bytes = (const uint8_t *)simplified_text,
                 .byte_count = sizeof(simplified_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:implicit-package"),
     .content = {.bytes = (const uint8_t *)resolved_text, .byte_count = sizeof(resolved_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/x/doc.xml"),
     .content = {.bytes = (const uint8_t *)document_text, .byte_count = sizeof(document_text) - 1}},
};

static const sxml_resource_entry_view fixture_resource_view = {
    .resource_entries = fixture_resources,
    .resource_entry_count = sizeof(fixture_resources) / sizeof(fixture_resources[0])};

/* What the trace sink kept: the messages' string values, joined by "|". */
typedef struct message_log {
    char text[256];
    size_t byte_count;
} message_log;

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    message_log messages;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = sxml_table_resource_resolver(&fixture_resource_view)},
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
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         test_fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* A name: Q{uri}local or a local name in no namespace. */
static sxml_name_identifier fixture_name(const fixture *test_fixture, sxml_byte_view text) {
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view local_name = text;
    if (text.byte_count > 2 && text.bytes[0] == 'Q') {
        const uint8_t *close = memchr(text.bytes, '}', text.byte_count);
        namespace_uri =
            (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = (size_t)(close - text.bytes) - 2};
        local_name = (sxml_byte_view){.bytes = close + 1,
                                      .byte_count = text.byte_count - (size_t)(close - text.bytes) - 1};
    }
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(test_fixture->processor);
    sxml_namespace_uri_identifier namespace_identifier = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_namespace_uri(name_pool, namespace_uri, &namespace_identifier) !=
            SXML_STATUS_OK ||
        sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_identifier, .local_name = local_name},
            &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

static sxml_sequence make_integer(const fixture *test_fixture, int64_t value) {
    sxml_item item = {.representation = 0};
    if (sxml_atomic_item_make(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_atomic_item_request){.type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                                        .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                                                  .datum_details = {.integer = value}}},
            &item) != SXML_STATUS_OK) {
        return sxml_sequence_empty();
    }
    return sxml_sequence_from_item(item);
}

/* Keeps each message's string value, as the text method writes it. */
static sxml_status log_message_bytes(void *state, sxml_byte_view bytes) {
    message_log *log = state;
    if (bytes.byte_count <= sizeof(log->text) - log->byte_count) {
        memcpy(log->text + log->byte_count, bytes.bytes, bytes.byte_count);
        log->byte_count += bytes.byte_count;
    }
    return SXML_STATUS_OK;
}

static const sxml_output_stream_operations message_log_operations = {.write = log_message_bytes};

static sxml_status receive_trace(void *state, const sxml_trace_event *event) {
    message_log *log = state;
    if (log->byte_count > 0) {
        (void)log_message_bytes(log, TEXT("|"));
    }
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_TEXT;
    return sxml_serialize_sequence(
        sxml_processor_get_item_heap(event->processor),
        &(sxml_serialization_request){.value = event->value,
                                      .serialization_parameters = parameters,
                                      .error_record = NULL},
        (sxml_output_stream){.state = log, .operations = &message_log_operations});
}

static const sxml_trace_sink_operations message_sink_operations = {.receive = receive_trace};

/* --------------------------------------------------------------- Running */

/* One transformation to run and what it gives. */
typedef struct transformation_run {
    sxml_byte_view stylesheet_text;
    /* The global context item's document; empty for none. */
    sxml_byte_view source_text;
    sxml_xslt_invocation_kind invocation_kind;
    /* Q{uri}local or a local name; empty for the default. */
    sxml_byte_view initial_template;
    sxml_byte_view initial_mode;
    sxml_byte_view initial_function;
    sxml_variable_binding_view static_parameters;
    sxml_variable_binding_view stylesheet_parameters;
    sxml_variable_binding_view template_parameters;
    sxml_variable_binding_view tunnel_parameters;
    sxml_sequence_view function_arguments;
    bool has_initial_match_selection;
    sxml_sequence initial_match_selection;
    sxml_xslt_result_delivery result_delivery;
    sxml_xslt_result_document_handler result_document_handler;
    /* Out: the error record of the failure; for RAW and TREE the value. */
    sxml_error_record error_record;
    sxml_sequence principal_result;
} transformation_run;

/* Compiles run->stylesheet_text (base URI http://example.com/x/main.xsl)
   and runs it; a serialized result goes to the fixture's output buffer
   (cleared first). */
static sxml_status run_transformation(fixture *test_fixture, transformation_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    run->principal_result = sxml_sequence_empty();
    test_fixture->messages.byte_count = 0;
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_text = run->stylesheet_text,
            .static_parameters = run->static_parameters,
            .static_context = {.static_base_uri = TEXT("http://example.com/x/main.xsl")},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &run->error_record},
        &stylesheet);
    sxml_item source = {.representation = 0};
    const bool has_source = run->source_text.byte_count > 0;
    if (status == SXML_STATUS_OK && has_source) {
        status = sxml_processor_parse_document(
            test_fixture->processor,
            &(sxml_xml_parse_request){.input = run->source_text,
                                      .source_name = TEXT("source.xml"),
                                      .document_uri = TEXT("http://example.com/x/source.xml"),
                                      .base_uri = TEXT("http://example.com/x/source.xml"),
                                      .encoding_name = {.bytes = NULL, .byte_count = 0},
                                      .diagnostic = NULL},
            &source);
    }
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof(dynamic_context));
        dynamic_context.has_context_value = has_source;
        dynamic_context.context_value =
            has_source ? sxml_sequence_from_item(source) : sxml_sequence_empty();
        dynamic_context.variable_bindings = run->stylesheet_parameters;
        /* The source is fn:doc's too, under its URI. */
        const sxml_available_document available = {
            .document_uri = TEXT("http://example.com/x/source.xml"), .document_node = source};
        if (has_source) {
            dynamic_context.available_documents = (sxml_available_document_view){
                .available_documents = &available, .available_document_count = 1};
        }
        dynamic_context.trace_sink = (sxml_trace_sink){.state = &test_fixture->messages,
                                                       .operations = &message_sink_operations};
        const bool is_serialized = run->result_delivery == SXML_XSLT_RESULT_DELIVERY_SERIALIZED;
        status = sxml_xslt_stylesheet_transform(
            stylesheet,
            &(sxml_xslt_transformation_request){
                .dynamic_context = dynamic_context,
                .initial_match_selection = run->initial_match_selection,
                .template_parameters = run->template_parameters,
                .tunnel_parameters = run->tunnel_parameters,
                .function_arguments = run->function_arguments,
                .base_output_uri = TEXT("http://example.com/out/main.xml"),
                .output_definition = NULL,
                .output_stream = is_serialized
                                     ? sxml_output_buffer_as_output_stream(test_fixture->output_buffer)
                                     : (sxml_output_stream){.state = NULL, .operations = NULL},
                .result_document_handler = run->result_document_handler,
                .error_record = &run->error_record,
                .initial_mode_name = run->initial_mode.byte_count > 0
                                         ? fixture_name(test_fixture, run->initial_mode)
                                         : (sxml_name_identifier){.index = 0},
                .initial_template_name = run->initial_template.byte_count > 0
                                             ? fixture_name(test_fixture, run->initial_template)
                                             : (sxml_name_identifier){.index = 0},
                .initial_function_name = run->initial_function.byte_count > 0
                                             ? fixture_name(test_fixture, run->initial_function)
                                             : (sxml_name_identifier){.index = 0},
                .invocation_kind = run->invocation_kind,
                .result_delivery = run->result_delivery,
                .has_initial_match_selection = run->has_initial_match_selection,
                .selects_unnamed_mode = false},
            &run->principal_result);
    }
    if (has_source && source.representation != 0) {
        sxml_item_release(source);
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

static bool output_is(const fixture *test_fixture, sxml_byte_view expected) {
    return sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                 expected);
}

static bool error_code_is(const transformation_run *run, sxml_byte_view local_name) {
    return sxml_byte_view_equals(run->error_record.code.local_name, local_name);
}

/* ------------------------------------------------- Transformation cases */

/* A stylesheet in the manner of an XSLT 4.0 test case (named after its
   test set) with its source and invocation, and its serialized output or
   the error code it raises. */
typedef struct transformation_case {
    const char *test_case_name;
    sxml_byte_view stylesheet_text;
    sxml_byte_view source_text;
    sxml_xslt_invocation_kind invocation_kind;
    sxml_byte_view initial_name;
    sxml_byte_view expected_output;
    sxml_byte_view expected_error_code;
} transformation_case;

#define NO_TEXT {.bytes = NULL, .byte_count = 0}

#define APPLY(name, stylesheet, source, output)                                            \
    {.test_case_name = (name),                                                           \
     .stylesheet_text = SXML_BYTE_VIEW_INITIALIZER(stylesheet),                          \
     .source_text = SXML_BYTE_VIEW_INITIALIZER(source),                                  \
     .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,                       \
     .initial_name = NO_TEXT,                                                            \
     .expected_output = SXML_BYTE_VIEW_INITIALIZER(output),                              \
     .expected_error_code = NO_TEXT}

#define CALL(name, stylesheet, template_name, output)                                      \
    {.test_case_name = (name),                                                           \
     .stylesheet_text = SXML_BYTE_VIEW_INITIALIZER(stylesheet),                          \
     .source_text = NO_TEXT,                                                             \
     .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,                         \
     .initial_name = SXML_BYTE_VIEW_INITIALIZER(template_name),                          \
     .expected_output = SXML_BYTE_VIEW_INITIALIZER(output),                              \
     .expected_error_code = NO_TEXT}

#define FAILS(name, stylesheet, source, kind, code)                                        \
    {.test_case_name = (name),                                                           \
     .stylesheet_text = SXML_BYTE_VIEW_INITIALIZER(stylesheet),                          \
     .source_text = SXML_BYTE_VIEW_INITIALIZER(source),                                  \
     .invocation_kind = (kind),                                                          \
     .initial_name = NO_TEXT,                                                            \
     .expected_output = NO_TEXT,                                                         \
     .expected_error_code = SXML_BYTE_VIEW_INITIALIZER(code)}

#define OMIT "<xsl:output omit-xml-declaration='yes'/>"
#define SOURCE "<doc><b>1</b><c>2</c><b>3</b></doc>"

static const transformation_case transformation_cases[] = {
    APPLY("apply-templates", STYLESHEET(OMIT "<xsl:template match='/'><out n='{count(//b)}'>"
                                             "<xsl:apply-templates select='doc/*'/></out>"
                                             "</xsl:template><xsl:template match='b'><B/>"
                                             "</xsl:template>"),
          SOURCE, "<out n=\"2\"><B/>2<B/></out>"),
    APPLY("built-in-templates", STYLESHEET("<xsl:output method='text'/>"), SOURCE, "123"),
    APPLY("mode", STYLESHEET(OMIT "<xsl:mode on-no-match='shallow-copy'/>"
                                  "<xsl:template match='c'/>"),
          SOURCE, "<doc><b>1</b><b>3</b></doc>"),
    /* typed="~T" (6.9.4): every item the mode processes is a T, else
       XTTE3100. */
    CALL("mode-typed-item-type",
         STYLESHEET("<xsl:output method='text'/><xsl:mode name='m' typed='~xs:integer'/>"
                    "<xsl:template match='atomic(xs:integer)' mode='m'>[<xsl:value-of "
                    "select='.'/>]</xsl:template><xsl:template name='xsl:initial-template'>"
                    "<xsl:apply-templates select='1, 2' mode='m'/></xsl:template>"),
         "", "[1][2]"),
    FAILS("mode-typed-item-type-refused",
          STYLESHEET("<xsl:mode name='m' typed=' ~ xs:integer'/><xsl:template "
                     "name='xsl:initial-template'><xsl:apply-templates select='1, \"x\"' "
                     "mode='m'/></xsl:template>"),
          "", SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE, "XTTE3100"),
    FAILS("mode-typed-unknown-type",
          STYLESHEET("<xsl:mode name='m' typed='~xs:nothing'/><xsl:template match='.' "
                     "mode='m'/>"),
          "", SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE, "XPST0051"),
    APPLY("priority", STYLESHEET(OMIT "<xsl:template match='/'><xsl:apply-templates "
                                      "select='//b'/></xsl:template><xsl:template match='b'>"
                                      "<low/></xsl:template><xsl:template match='b[. = 3]'>"
                                      "<high/></xsl:template>"),
          SOURCE, "<low/><high/>"),
    APPLY("sort", STYLESHEET("<xsl:output method='text'/><xsl:template match='/'><xsl:for-each "
                             "select='//*[not(*)]'><xsl:sort select='.' order='descending'/>"
                             "<xsl:value-of select='.'/></xsl:for-each></xsl:template>"),
          SOURCE, "321"),
    APPLY("for-each-group", STYLESHEET("<xsl:output method='text'/><xsl:template match='/'>"
                                       "<xsl:for-each-group select='doc/*' group-by='name()'>"
                                       "<xsl:value-of select='current-grouping-key(), "
                                       "sum(current-group())'/>;</xsl:for-each-group>"
                                       "</xsl:template>"),
          SOURCE, "b 4;c 2;"),
    APPLY("key", STYLESHEET("<xsl:output method='text'/><xsl:key name='k' match='*' "
                            "use='string(.)'/><xsl:template match='/'><xsl:value-of "
                            "select=\"name(key('k', '2'))\"/></xsl:template>"),
          SOURCE, "c"),
    APPLY("number", STYLESHEET("<xsl:output method='text'/><xsl:template match='b'><xsl:number "
                               "format='(a)'/></xsl:template><xsl:template match='c'/>"),
          SOURCE, "(a)(b)"),
    APPLY("choose", STYLESHEET("<xsl:output method='text'/><xsl:template match='/'><xsl:for-each "
                               "select='doc/*'><xsl:choose><xsl:when test='. = 2'>two</xsl:when>"
                               "<xsl:otherwise>other</xsl:otherwise></xsl:choose></xsl:for-each>"
                               "</xsl:template>"),
          SOURCE, "othertwoother"),
    APPLY("copy", STYLESHEET(OMIT "<xsl:template match='/'><xsl:copy-of select='//c'/><xsl:for-each "
                                  "select='//b[1]'><xsl:copy><xsl:attribute name='x'>y"
                                  "</xsl:attribute></xsl:copy></xsl:for-each></xsl:template>"),
          SOURCE, "<c>2</c><b x=\"y\"/>"),
    APPLY("function", STYLESHEET("<xsl:output method='text'/><xsl:function name='f:twice'><xsl:param "
                                 "name='n' as='xs:integer'/><xsl:sequence select='$n * 2'/>"
                                 "</xsl:function><xsl:template match='/'><xsl:value-of "
                                 "select='f:twice(count(//b))'/></xsl:template>"),
          SOURCE, "4"),
    APPLY("variable", STYLESHEET("<xsl:output method='text'/><xsl:variable name='total' "
                                 "select='sum(//*[not(*)])'/><xsl:param name='scale' select='10'/>"
                                 "<xsl:template match='/'><xsl:value-of select='$total * $scale'/>"
                                 "</xsl:template>"),
          SOURCE, "60"),
    APPLY("include-import", STYLESHEET(OMIT "<xsl:import href='imported.xsl'/><xsl:include "
                                            "href='included.xsl'/><xsl:template match='/'><out>"
                                            "<xsl:call-template name='included'/><xsl:apply-templates "
                                            "select='doc/*'/></out></xsl:template><xsl:template "
                                            "match='c'><own/></xsl:template>"),
          SOURCE, "<out><inc/><imported/><own/><imported/></out>"),
    APPLY("strip-space", STYLESHEET("<xsl:output method='text'/><xsl:strip-space elements='*'/>"
                                    "<xsl:template match='/'><xsl:value-of "
                                    "select='count(doc/node())'/></xsl:template>"),
          "<doc> <b/> <b/> </doc>", "2"),
    /* 4.2.2: an available document is a source document too, and the
       context item that is one stays one tree with fn:doc's. */
    APPLY("strip-space-available",
          STYLESHEET("<xsl:output method='text'/><xsl:strip-space elements='*'/>"
                     "<xsl:template match='/'><xsl:value-of "
                     "select=\"count(doc('source.xml')/doc/node()), doc('source.xml') is /\"/>"
                     "</xsl:template>"),
          "<doc> <b/> <b/> </doc>", "2 true"),
    APPLY("document", STYLESHEET("<xsl:output method='text'/><xsl:template match='/'><xsl:value-of "
                                 "select=\"sum(document('doc.xml')//v)\"/></xsl:template>"),
          SOURCE, "3"),
    APPLY("result-document-principal",
          STYLESHEET("<xsl:template match='/'><xsl:result-document method='text'>"
                     "<xsl:value-of select='count(//b)'/></xsl:result-document></xsl:template>"),
          SOURCE, "2"),
    APPLY("analyze-string", STYLESHEET("<xsl:output method='text'/><xsl:template match='/'>"
                                       "<xsl:analyze-string select=\"'a1b22'\" regex='[0-9]+'>"
                                       "<xsl:matching-substring>[<xsl:value-of select='.'/>]"
                                       "</xsl:matching-substring><xsl:non-matching-substring>"
                                       "<xsl:value-of select='upper-case(.)'/>"
                                       "</xsl:non-matching-substring></xsl:analyze-string>"
                                       "</xsl:template>"),
          SOURCE, "A[1]B[22]"),
    CALL("initial-template", STYLESHEET(OMIT "<xsl:template name='xsl:initial-template'><out "
                                             "n='{1 + 1}'/></xsl:template>"),
         "", "<out n=\"2\"/>"),
    CALL("call-template", STYLESHEET("<xsl:output method='text'/><xsl:template name='main'>"
                                     "<xsl:iterate select='1 to 5'><xsl:param name='sum' "
                                     "select='0'/><xsl:on-completion><xsl:value-of select='$sum'/>"
                                     "</xsl:on-completion><xsl:next-iteration><xsl:with-param "
                                     "name='sum' select='$sum + .'/></xsl:next-iteration>"
                                     "</xsl:iterate></xsl:template>"),
         "main", "15"),
    CALL("output-html", STYLESHEET("<xsl:output method='html'/><xsl:template "
                                   "name='xsl:initial-template'><p>a<br/></p></xsl:template>"),
         "", "<p>a<br></p>"),
    FAILS("no-match-selection", STYLESHEET("<xsl:template match='/'/><xsl:mode name='m'/>"), SOURCE,
          SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTDE0044"),
    FAILS("no-initial-template", STYLESHEET("<xsl:template match='/'/>"), "",
          SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE, "XTDE0040"),
    FAILS("terminate", STYLESHEET("<xsl:template match='/'><xsl:message terminate='yes'>stop"
                                  "</xsl:message></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTMM9000"),
    FAILS("required-param", STYLESHEET("<xsl:param name='p' required='yes'/><xsl:template "
                                       "match='/'><xsl:value-of select='$p'/></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTDE0050"),
    FAILS("static-error", STYLESHEET("<xsl:template match='/'><xsl:value-of select='1 +'/>"
                                     "</xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XPST0003"),
    FAILS("unknown-element", STYLESHEET("<xsl:template match='/'><xsl:no-such/></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTSE0010"),
    FAILS("not-in-scope", STYLESHEET("<xsl:template match='/'><xsl:value-of select='$v'/>"
                                     "</xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XPST0008"),
    /* 10.5: xsl:evaluate compiles at run time; parameters from
       xsl:with-param and with-params (which wins), the context item, the
       instruction's namespaces (an inner xmlns too) and base URI. */
    APPLY("evaluate", STYLESHEET(OMIT "<xsl:template match='/'><out><xsl:evaluate "
                                      "xpath=\"'count(//b) + $n + $m'\" context-item='.' "
                                      "with-params=\"map{xs:QName('m'): 100}\">"
                                      "<xsl:with-param name='n' select='10'/>"
                                      "<xsl:with-param name='m' select='1000'/>"
                                      "</xsl:evaluate></out></xsl:template>"),
          SOURCE, "<out>112</out>"),
    APPLY("evaluate-static-context",
          STYLESHEET(OMIT "<xsl:template match='/'><out xmlns:p='urn:p'><xsl:evaluate "
                          "xpath=\"'namespace-uri-from-QName(xs:QName(&quot;p:a&quot;)), "
                          "static-base-uri()'\" base-uri='http://example.com/b/'/>"
                          "</out></xsl:template>"),
          SOURCE, "<out xmlns:p=\"urn:p\">urn:p http://example.com/b/</out>"),
    APPLY("evaluate-as", STYLESHEET(OMIT "<xsl:template match='/'><out><xsl:evaluate "
                                         "xpath=\"'(1, 2)'\" as='xs:double*'/></out>"
                                         "</xsl:template>"),
          SOURCE, "<out>1 2</out>"),
    /* Public stylesheet functions are available, and an xsl:evaluate in
       one runs nested in the target expression without C recursion (a
       unit evaluation on the machine, D71); fn:current is the context
       item. */
    APPLY("evaluate-functions",
          STYLESHEET(OMIT "<xsl:function name='f:twice' visibility='public'>"
                          "<xsl:param name='x'/><xsl:evaluate xpath=\"'$x * 2'\">"
                          "<xsl:with-param name='x' select='$x'/></xsl:evaluate></xsl:function>"
                          "<xsl:template match='/'><out><xsl:evaluate "
                          "xpath=\"'f:twice(f:twice(5)), f:twice#1(1), count(current()/doc/b)'\" "
                          "context-item='.'/></out></xsl:template>"),
          SOURCE, "<out>20 2 2</out>"),
    FAILS("evaluate-private-function",
          STYLESHEET("<xsl:function name='f:hidden'><xsl:sequence select='1'/></xsl:function>"
                     "<xsl:template match='/'><xsl:evaluate xpath=\"'f:hidden()'\"/>"
                     "</xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTDE3160"),
    FAILS("evaluate-xslt-function",
          STYLESHEET("<xsl:template match='/'><xsl:evaluate "
                     "xpath=\"'system-property(&quot;xsl:version&quot;)'\"/></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTDE3160"),
    FAILS("evaluate-invalid", STYLESHEET("<xsl:template match='/'><xsl:evaluate "
                                         "xpath=\"'1 +'\"/></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTDE3160"),
    FAILS("evaluate-no-context", STYLESHEET("<xsl:template match='/'><xsl:evaluate "
                                            "xpath=\"'.'\"/></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XPDY0002"),
    FAILS("evaluate-with-params", STYLESHEET("<xsl:template match='/'><xsl:evaluate "
                                             "xpath=\"'1'\" with-params='1'/></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTTE3165"),
    FAILS("evaluate-namespace-context",
          STYLESHEET("<xsl:template match='/'><xsl:evaluate xpath=\"'1'\" "
                     "namespace-context='1'/></xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "XTTE3170"),
    FAILS("evaluate-untrusted", STYLESHEET("<xsl:template match='/'><xsl:evaluate "
                                           "xpath=\"'doc(&quot;a.xml&quot;)'\"/>"
                                           "</xsl:template>"),
          SOURCE, SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES, "FODC0016"),
};

static void test_transformations_round_trip(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0;
         case_index < sizeof(transformation_cases) / sizeof(transformation_cases[0]);
         case_index += 1) {
        const transformation_case *current = &transformation_cases[case_index];
        transformation_run run = {.stylesheet_text = current->stylesheet_text,
                                  .source_text = current->source_text,
                                  .invocation_kind = current->invocation_kind,
                                  .initial_template = current->initial_name,
                                  .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED};
        if (sxml_byte_view_equals(current->expected_error_code, TEXT("XTDE0044"))) {
            /* The no-match-selection case: mode m, without a source. */
            run.initial_mode = TEXT("m");
            run.source_text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        }
        const sxml_status status = run_transformation(&test_fixture, &run);
        sxml_sequence_release(run.principal_result);
        const bool passed = current->expected_error_code.byte_count > 0
                                ? status != SXML_STATUS_OK &&
                                      error_code_is(&run, current->expected_error_code)
                                : status == SXML_STATUS_OK &&
                                      output_is(&test_fixture, current->expected_output);
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression =
                                                                  current->test_case_name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------ Parameters */

static void test_parameters_reach_their_declarations(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_sequence one = make_integer(&test_fixture, 1);
    const sxml_sequence two = make_integer(&test_fixture, 2);
    const sxml_sequence three = make_integer(&test_fixture, 3);
    const sxml_variable_binding static_bindings[] = {
        {.variable_name = fixture_name(&test_fixture, TEXT("s")), .value = one}};
    const sxml_variable_binding stylesheet_bindings[] = {
        {.variable_name = fixture_name(&test_fixture, TEXT("g")), .value = two}};
    const sxml_variable_binding template_bindings[] = {
        {.variable_name = fixture_name(&test_fixture, TEXT("t")), .value = three}};
    const sxml_variable_binding tunnel_bindings[] = {
        {.variable_name = fixture_name(&test_fixture, TEXT("u")), .value = one}};
    transformation_run run = {
        .stylesheet_text = TEXT(STYLESHEET(
            "<xsl:output method='text'/><xsl:param name='s' static='yes' as='xs:integer' "
            "select='0'/><xsl:param name='g' as='xs:integer' select='0'/>"
            "<xsl:template name='main' use-when='$s = 1'><xsl:param name='t'/><xsl:value-of "
            "select='$s, $g, $t'/><xsl:call-template name='inner'/></xsl:template>"
            "<xsl:template name='inner'><xsl:param name='u' tunnel='yes'/><xsl:value-of "
            "select=\"'/' || $u\"/></xsl:template>")),
        .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
        .initial_template = TEXT("main"),
        .static_parameters = {.variable_bindings = static_bindings, .variable_binding_count = 1},
        .stylesheet_parameters = {.variable_bindings = stylesheet_bindings,
                                  .variable_binding_count = 1},
        .template_parameters = {.variable_bindings = template_bindings,
                                .variable_binding_count = 1},
        .tunnel_parameters = {.variable_bindings = tunnel_bindings, .variable_binding_count = 1},
        .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED};
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("1 2 3/1")));
    /* A parameter the template does not declare is ignored (2.4.4). */
    run.template_parameters = run.tunnel_parameters;
    run.tunnel_parameters = (sxml_variable_binding_view){.variable_bindings = NULL,
                                                         .variable_binding_count = 0};
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("1 2 /")));
    sxml_sequence_release(one);
    sxml_sequence_release(two);
    sxml_sequence_release(three);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_initial_function_and_selection(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_sequence arguments[] = {make_integer(&test_fixture, 20),
                                       make_integer(&test_fixture, 22)};
    transformation_run run = {
        .stylesheet_text = TEXT(STYLESHEET(
            OMIT "<xsl:function name='f:add' visibility='public' as='xs:integer'><xsl:param "
            "name='a' as='xs:integer'/><xsl:param name='b' as='xs:integer'/><xsl:sequence "
            "select='$a + $b'/></xsl:function><xsl:function name='f:hidden'><xsl:sequence "
            "select='0'/></xsl:function><xsl:template match='.'><xsl:sequence select='. * 2'/>"
            "</xsl:template>")),
        .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION,
        .initial_function = TEXT("Q{urn:f}add"),
        .function_arguments = {.sequences = arguments, .sequence_count = 2},
        .result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW};
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    SXML_TEST_EXPECT(state, sxml_sequence_get_item(run.principal_result,
                                                   (sxml_item_request){.item_index = 0},
                                                   &item) == SXML_STATUS_OK &&
                                sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
                                datum.datum_details.integer == 42);
    sxml_sequence_release(run.principal_result);
    /* A private function, or another arity, is no initial function. */
    run.initial_function = TEXT("Q{urn:f}hidden");
    run.function_arguments.sequence_count = 0;
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XTDE0041")));
    run.initial_function = TEXT("Q{urn:f}add");
    run.function_arguments.sequence_count = 1;
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XTDE0041")));
    /* An initial match selection of atomic items (2.4.3). */
    sxml_sequence selection = sxml_sequence_empty();
    SXML_TEST_EXPECT(state,
                     sxml_sequence_make_integer_range(
                         sxml_processor_get_item_heap(test_fixture.processor),
                         (sxml_integer_range){.first_integer = 1, .last_integer = 3}, &selection) ==
                         SXML_STATUS_OK);
    run.invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES;
    run.initial_function = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    run.function_arguments.sequence_count = 0;
    run.has_initial_match_selection = true;
    run.initial_match_selection = selection;
    run.result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED;
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("2 4 6")));
    sxml_sequence_release(selection);
    sxml_sequence_release(arguments[0]);
    sxml_sequence_release(arguments[1]);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ----------------------------------------------------- Results, messages */

static void test_deliveries_documents_and_messages(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_xslt_result_document_collector *collector = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_xslt_result_document_collector_create(
                         fixture_allocator(&test_fixture),
                         &(sxml_xslt_result_document_collector_configuration){
                             .allocator = fixture_allocator(&test_fixture),
                             .maximum_byte_count = 0,
                             .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
                         &collector) == SXML_STATUS_OK);
    transformation_run run = {
        .stylesheet_text = TEXT(STYLESHEET(
            "<xsl:template match='/'><xsl:message>one</xsl:message><xsl:result-document "
            "href='part.txt' method='text'>part</xsl:result-document><xsl:message "
            "select=\"'two'\"/><r><xsl:value-of select='count(//b)'/></r></xsl:template>")),
        .source_text = TEXT(SOURCE),
        .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,
        .result_delivery = SXML_XSLT_RESULT_DELIVERY_TREE,
        .result_document_handler = sxml_xslt_result_document_collector_as_handler(collector)};
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_OK);
    /* TREE: one document node. */
    sxml_item item = {.representation = 0};
    sxml_node_reference node;
    sxml_node_description description;
    SXML_TEST_EXPECT(state,
                     sxml_sequence_get_item(run.principal_result,
                                            (sxml_item_request){.item_index = 0}, &item) ==
                             SXML_STATUS_OK &&
                         sxml_item_get_node(item, &node) == SXML_STATUS_OK &&
                         sxml_node_describe(node, &description) == SXML_STATUS_OK &&
                         description.node_kind == SXML_NODE_KIND_DOCUMENT);
    sxml_sequence_release(run.principal_result);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                (sxml_byte_view){.bytes = (const uint8_t *)test_fixture.messages.text,
                                                 .byte_count = test_fixture.messages.byte_count},
                                TEXT("one|two")));
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_get_result_document_count(
                                collector) == 1);
    sxml_xslt_collected_result_document document;
    memset(&document, 0, sizeof(document));
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_get_result_document(
                                collector, 0, &document) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(document.absolute_uri,
                                                  TEXT("http://example.com/out/part.txt")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(document.serialized_bytes, TEXT("part")));
    sxml_xslt_result_document_collector_clear(collector);
    /* RAW: the sequence itself; two result documents of one URI fail. */
    run.stylesheet_text = TEXT(STYLESHEET(
        "<xsl:template name='xsl:initial-template'><xsl:sequence select='1, 2'/>"
        "</xsl:template><xsl:template name='twice'><xsl:result-document href='a'/>"
        "<xsl:result-document href='a'/></xsl:template>"));
    run.source_text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    run.invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE;
    run.result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW;
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_OK);
    uint64_t item_count = 0;
    SXML_TEST_EXPECT(state, sxml_sequence_count(run.principal_result, NULL, &item_count) ==
                                    SXML_STATUS_OK &&
                                item_count == 2);
    sxml_sequence_release(run.principal_result);
    run.initial_template = TEXT("twice");
    SXML_TEST_EXPECT(state, run_transformation(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XTDE1490")));
    sxml_xslt_result_document_collector_destroy(collector);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* The collector on caller memory (initialize, finalize), its argument
   checks and an empty collector's reads. */
static void test_collector_lifecycle_checks_its_arguments(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_xslt_result_document_collector_configuration configuration = {
        .allocator = fixture_allocator(&test_fixture),
        .maximum_byte_count = 0,
        .result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW};
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_memory_requirements(
                                &configuration, &requirements) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count > 0 && requirements.byte_count <= 1024 &&
                                requirements.alignment <= 16);
    alignas(16) uint8_t storage[1024 + 16];
    sxml_xslt_result_document_collector *collector = NULL;
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_initialize(
                                (sxml_memory_view){.bytes = storage,
                                                   .byte_count = requirements.byte_count - 1},
                                &configuration,
                                &collector) == SXML_STATUS_INSUFFICIENT_MEMORY);
    if (requirements.alignment > 1) {
        SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_initialize(
                                    (sxml_memory_view){.bytes = storage + 1,
                                                       .byte_count = requirements.byte_count},
                                    &configuration,
                                    &collector) == SXML_STATUS_MISALIGNED_MEMORY);
    }
    SXML_TEST_EXPECT(state, collector == NULL);
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_initialize(
                                (sxml_memory_view){.bytes = storage,
                                                   .byte_count = requirements.byte_count},
                                &configuration, &collector) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_xslt_result_document_collector_get_result_document_count(collector) == 0);
    sxml_xslt_collected_result_document document;
    memset(&document, 0, sizeof(document));
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_get_result_document(
                                collector, 0, &document) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_xslt_result_document_collector_finalize(collector);
    sxml_xslt_result_document_collector_finalize(NULL);
    sxml_xslt_result_document_collector_destroy(NULL);
    /* The null allocator. */
    configuration.allocator = (sxml_allocator){.state = NULL, .operations = NULL};
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_memory_requirements(
                                &configuration, &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_xslt_result_document_collector_create(
                                fixture_allocator(&test_fixture), &configuration,
                                &collector) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_output_definitions_are_read(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_xslt_stylesheet *stylesheet = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_xslt_stylesheet_create(
                         fixture_allocator(&test_fixture),
                         &(sxml_xslt_stylesheet_configuration){
                             .processor = test_fixture.processor,
                             .principal_module_text = TEXT(STYLESHEET(
                                 "<xsl:output method='json' indent='yes'/>"
                                 "<xsl:output name='f:plain' method='text'/>")),
                             .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT},
                         &stylesheet) == SXML_STATUS_OK);
    sxml_xslt_output_definition definition;
    memset(&definition, 0, sizeof(definition));
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_get_output_definition(
                                stylesheet, (sxml_name_identifier){.index = 0}, &definition) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, definition.has_serialization_method &&
                                definition.serialization_parameters.serialization_method ==
                                    SXML_SERIALIZATION_METHOD_JSON &&
                                definition.serialization_parameters.indent ==
                                    SXML_SERIALIZATION_FLAG_YES);
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_get_output_definition(
                                stylesheet, fixture_name(&test_fixture, TEXT("Q{urn:f}plain")),
                                &definition) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, definition.serialization_parameters.serialization_method ==
                                SXML_SERIALIZATION_METHOD_TEXT);
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_get_output_definition(
                                stylesheet, fixture_name(&test_fixture, TEXT("none")),
                                &definition) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_xslt_stylesheet_destroy(stylesheet);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------- Modules from resolver */

/* Compiles the principal module the resolver delivers and calls
   xsl:initial-template. */
typedef struct resolved_principal {
    sxml_xslt_principal_module_source principal_module_source;
    sxml_byte_view uri_reference;
} resolved_principal;

static sxml_status run_resolved(fixture *test_fixture, resolved_principal principal,
                                sxml_error_record *error_record) {
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_uri_reference = principal.uri_reference,
            .static_context = {.static_base_uri = TEXT("http://example.com/x/")},
            .principal_module_source = principal.principal_module_source,
            .error_record = error_record},
        &stylesheet);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_stylesheet_transform(
            stylesheet,
            &(sxml_xslt_transformation_request){
                .output_stream = sxml_output_buffer_as_output_stream(test_fixture->output_buffer),
                .error_record = error_record,
                .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
            &result);
        sxml_sequence_release(result);
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

static void test_principal_module_from_the_resolver(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    SXML_TEST_EXPECT(state, run_resolved(&test_fixture,
                                         (resolved_principal){
                                             .principal_module_source =
                                                 SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_MODULE,
                                             .uri_reference = TEXT("resolved.xsl")},
                                         &error_record) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture,
                                      TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?><resolved/>")));
    SXML_TEST_EXPECT(state, run_resolved(&test_fixture,
                                         (resolved_principal){
                                             .principal_module_source =
                                                 SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE,
                                             .uri_reference = TEXT("urn:p")},
                                         &error_record) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture,
                                      TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?><package/>")));
    /* An xsl:stylesheet is an implicit package (XSLT 4.0, 3.4; D160). */
    SXML_TEST_EXPECT(state, run_resolved(&test_fixture,
                                         (resolved_principal){
                                             .principal_module_source =
                                                 SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE,
                                             .uri_reference = TEXT("urn:implicit-package")},
                                         &error_record) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture,
                                      TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?><resolved/>")));
    SXML_TEST_EXPECT(state, run_resolved(&test_fixture,
                                         (resolved_principal){
                                             .principal_module_source =
                                                 SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_MODULE,
                                             .uri_reference = TEXT("missing.xsl")},
                                         &error_record) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name, TEXT("XTSE0165")));
    SXML_TEST_EXPECT(state, run_resolved(&test_fixture,
                                         (resolved_principal){
                                             .principal_module_source =
                                                 SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE,
                                             .uri_reference = TEXT("urn:not-a-package")},
                                         &error_record) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name, TEXT("XTSE3000")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A dynamic error inside an included module names that module (D169):
   in a template and in a stylesheet function. The record is read before
   the stylesheet goes (include/sxml/status.h, D21). */
static void test_errors_name_their_module(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char *const stylesheets[] = {
        STYLESHEET("<xsl:include href='failing.xsl'/><xsl:template "
                   "name='xsl:initial-template'><xsl:call-template name='fails'/></xsl:template>"),
        STYLESHEET("<xsl:include href='failing.xsl'/><xsl:template "
                   "name='xsl:initial-template'><xsl:sequence select='f:fails()'/></xsl:template>")};
    for (size_t stylesheet_index = 0; stylesheet_index < 2; stylesheet_index += 1) {
        sxml_error_record error_record;
        memset(&error_record, 0, sizeof(error_record));
        sxml_xslt_stylesheet *stylesheet = NULL;
        sxml_status status = sxml_xslt_stylesheet_create(
            fixture_allocator(&test_fixture),
            &(sxml_xslt_stylesheet_configuration){
                .processor = test_fixture.processor,
                .principal_module_text = sxml_byte_view_from_c_string(stylesheets[stylesheet_index]),
                .static_context = {.static_base_uri = TEXT("http://example.com/x/main.xsl")},
                .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
                .error_record = &error_record},
            &stylesheet);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        sxml_sequence result = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            sxml_dynamic_context_configuration dynamic_context;
            memset(&dynamic_context, 0, sizeof(dynamic_context));
            status = sxml_xslt_stylesheet_transform(
                stylesheet,
                &(sxml_xslt_transformation_request){
                    .dynamic_context = dynamic_context,
                    .error_record = &error_record,
                    .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                    .result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW},
                &result);
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_DYNAMIC_ERROR);
        const sxml_source_location location = error_record.diagnostic.location;
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(location.source_name,
                                                      TEXT("http://example.com/x/failing.xsl")));
        SXML_TEST_EXPECT(state, location.line_number == 2);
        sxml_sequence_release(result);
        sxml_xslt_stylesheet_destroy(stylesheet);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------- Lifecycle */

static void test_stylesheet_lifecycle_checks_its_arguments(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_memory_requirements(
                                &(sxml_xslt_stylesheet_configuration){.processor = NULL},
                                &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_xslt_stylesheet_configuration configuration = {
        .processor = test_fixture.processor,
        .principal_module_text =
            TEXT(STYLESHEET("<xsl:template name='xsl:initial-template'>x</xsl:template>"))};
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_memory_requirements(&configuration,
                                                                     &requirements) ==
                                SXML_STATUS_OK);
    alignas(max_align_t) uint8_t memory[512];
    SXML_TEST_EXPECT(state, requirements.byte_count <= sizeof(memory));
    sxml_xslt_stylesheet *stylesheet = NULL;
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_initialize(
                                (sxml_memory_view){.bytes = memory, .byte_count = 1},
                                &configuration, &stylesheet) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state,
                     sxml_xslt_stylesheet_initialize(
                         (sxml_memory_view){.bytes = memory, .byte_count = sizeof(memory)},
                         &configuration, &stylesheet) == SXML_STATUS_OK);
    sxml_sequence result = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_transform(stylesheet, NULL, &result) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    /* Fields of another invocation kind, a context value of two items,
       a serialized delivery without an output stream. */
    SXML_TEST_EXPECT(state,
                     sxml_xslt_stylesheet_transform(
                         stylesheet,
                         &(sxml_xslt_transformation_request){
                             .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                             .has_initial_match_selection = true},
                         &result) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_sequence two = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_sequence_make_integer_range(
                                sxml_processor_get_item_heap(test_fixture.processor),
                                (sxml_integer_range){.first_integer = 1, .last_integer = 2},
                                &two) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_xslt_stylesheet_transform(
                         stylesheet,
                         &(sxml_xslt_transformation_request){
                             .dynamic_context = {.has_context_value = true, .context_value = two},
                             .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE},
                         &result) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_sequence_release(two);
    SXML_TEST_EXPECT(state,
                     sxml_xslt_stylesheet_transform(
                         stylesheet,
                         &(sxml_xslt_transformation_request){
                             .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                             .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
                         &result) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_xslt_stylesheet_transform(
                         stylesheet,
                         &(sxml_xslt_transformation_request){
                             .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE},
                         &result) == SXML_STATUS_OK);
    sxml_sequence_release(result);
    sxml_xslt_stylesheet_finalize(stylesheet);
    sxml_xslt_stylesheet_finalize(NULL);
    sxml_xslt_stylesheet_destroy(NULL);
    /* The stylesheet declares its namespaces itself. */
    const sxml_namespace_binding bindings[] = {
        {.namespace_prefix = {.index = 0}, .namespace_uri = {.index = 0}}};
    sxml_xslt_stylesheet_configuration invalid = configuration;
    invalid.static_context.namespace_bindings =
        (sxml_namespace_binding_view){.namespace_bindings = bindings, .namespace_binding_count = 1};
    SXML_TEST_EXPECT(state, sxml_xslt_stylesheet_create(fixture_allocator(&test_fixture),
                                                        &invalid, &stylesheet) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Compiles and runs one stylesheet with allocation failing_number
   failing. */
static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        transformation_run run = {
            .stylesheet_text = TEXT(STYLESHEET(
                "<xsl:output method='text'/><xsl:strip-space elements='*'/>"
                "<xsl:include href='included.xsl'/><xsl:variable "
                "name='n' select='count(//b)'/><xsl:template match='/'><xsl:apply-templates "
                "select='doc/*'/><xsl:value-of select='$n'/></xsl:template><xsl:template "
                "match='b'>[<xsl:value-of select='.'/>]</xsl:template>")),
            .source_text = TEXT(SOURCE),
            .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,
            .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED};
        status = run_transformation(&test_fixture, &run);
        sxml_sequence_release(run.principal_result);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("[1]2[3]2")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_transformation_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 200000; failing_number += 1) {
        status = run_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case xslt_test_cases[] = {
    {.name = "transformations_round_trip", .function = test_transformations_round_trip},
    {.name = "parameters_reach_their_declarations",
     .function = test_parameters_reach_their_declarations},
    {.name = "initial_function_and_selection", .function = test_initial_function_and_selection},
    {.name = "deliveries_documents_and_messages",
     .function = test_deliveries_documents_and_messages},
    {.name = "collector_lifecycle_checks_its_arguments",
     .function = test_collector_lifecycle_checks_its_arguments},
    {.name = "output_definitions_are_read", .function = test_output_definitions_are_read},
    {.name = "principal_module_from_the_resolver",
     .function = test_principal_module_from_the_resolver},
    {.name = "errors_name_their_module", .function = test_errors_name_their_module},
    {.name = "stylesheet_lifecycle_checks_its_arguments",
     .function = test_stylesheet_lifecycle_checks_its_arguments},
    {.name = "transformation_survives_every_allocation_failure",
     .function = test_transformation_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt",
            .cases = xslt_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
