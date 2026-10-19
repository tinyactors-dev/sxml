/*
 * Tests of the XML parser (include/sxml/xml.h; packet 1.14, D81).
 *
 * Golden file: tests/memory_test.c (layout, naming, allocation-failure
 * loops).
 *
 * Most cases parse a small document into a builder that writes each event
 * as text, and compare the text:
 *
 *     D                 start_document      /D   end_document
 *     U(name,sys,pub)   unparsed entity
 *     <{uri}local       start_element       </>  end_element
 *      ns(p=uri)        namespace binding   (prefix "" for the default)
 *      @{uri}local=v    attribute, then !id or !refs for the ID properties
 *     'text'            text                (!c)  comment
 *     (?target data)    processing instruction
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

enum { SXML_TEST_LOG_BYTE_COUNT = 4096 };

/* ------------------------------------------------------------ Event log */

/* A builder that writes events as text. */
typedef struct event_log {
    const sxml_name_pool *name_pool;
    char text[SXML_TEST_LOG_BYTE_COUNT];
    size_t byte_count;
    /* The location of the last start_element, for location tests. */
    sxml_source_location element_location;
} event_log;

static void log_append_view(event_log *log, sxml_byte_view view) {
    for (size_t byte_index = 0; byte_index < view.byte_count &&
                                log->byte_count + 1 < SXML_TEST_LOG_BYTE_COUNT;
         byte_index++) {
        log->text[log->byte_count] = (char)view.bytes[byte_index];
        log->byte_count += 1;
    }
    log->text[log->byte_count] = '\0';
}

static void log_append(event_log *log, const char *text) {
    log_append_view(log, sxml_byte_view_from_c_string(text));
}

/* Appends {uri}local, or local for a name in no namespace. */
static sxml_status log_append_name(event_log *log, sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status =
        sxml_name_pool_get_name_parts(log->name_pool, name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (parts.namespace_uri.index != 0) {
        sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
        status = sxml_name_pool_get_namespace_uri(log->name_pool,
                                                  parts.namespace_uri, &uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        log_append(log, "{");
        log_append_view(log, uri);
        log_append(log, "}");
    }
    log_append_view(log, parts.local_name);
    return SXML_STATUS_OK;
}

static event_log *log_from_state(void *state) { return state; }

static sxml_status log_start_document(void *state,
                                      const sxml_document_start_event *event) {
    (void)event;
    log_append(log_from_state(state), "D");
    return SXML_STATUS_OK;
}

static sxml_status log_end_document(void *state) {
    log_append(log_from_state(state), "/D");
    return SXML_STATUS_OK;
}

static sxml_status log_start_element(void *state,
                                     const sxml_element_start_event *event) {
    event_log *log = log_from_state(state);
    log->element_location = event->location;
    log_append(log, "<");
    return log_append_name(log, event->element_name);
}

static sxml_status
log_add_namespace_binding(void *state, const sxml_namespace_binding *binding) {
    event_log *log = log_from_state(state);
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_name_pool_get_namespace_prefix(
        log->name_pool, binding->namespace_prefix, &prefix);
    if (status == SXML_STATUS_OK && binding->namespace_uri.index != 0) {
        status = sxml_name_pool_get_namespace_uri(log->name_pool,
                                                  binding->namespace_uri, &uri);
    }
    log_append(log, " ns(");
    log_append_view(log, prefix);
    log_append(log, "=");
    log_append_view(log, uri);
    log_append(log, ")");
    return status;
}

static sxml_status log_add_attribute(void *state,
                                     const sxml_attribute_event *event) {
    event_log *log = log_from_state(state);
    log_append(log, " @");
    sxml_status status = log_append_name(log, event->attribute_name);
    log_append(log, "=");
    log_append_view(log, event->attribute_value);
    if (event->is_id) {
        log_append(log, "!id");
    }
    if (event->is_idrefs) {
        log_append(log, "!refs");
    }
    return status;
}

static sxml_status log_end_element(void *state) {
    log_append(log_from_state(state), "</>");
    return SXML_STATUS_OK;
}

static sxml_status log_add_text(void *state, const sxml_text_event *event) {
    event_log *log = log_from_state(state);
    log_append(log, "'");
    log_append_view(log, event->text);
    log_append(log, "'");
    return SXML_STATUS_OK;
}

static sxml_status log_add_comment(void *state,
                                   const sxml_comment_event *event) {
    event_log *log = log_from_state(state);
    log_append(log, "(!");
    log_append_view(log, event->comment);
    log_append(log, ")");
    return SXML_STATUS_OK;
}

static sxml_status
log_add_processing_instruction(void *state,
                               const sxml_processing_instruction_event *event) {
    event_log *log = log_from_state(state);
    log_append(log, "(?");
    log_append_view(log, event->target);
    log_append(log, " ");
    log_append_view(log, event->data);
    log_append(log, ")");
    return SXML_STATUS_OK;
}

static sxml_status log_add_unparsed_entity(void *state,
                                           const sxml_unparsed_entity *entity) {
    event_log *log = log_from_state(state);
    log_append(log, "U(");
    log_append_view(log, entity->entity_name);
    log_append(log, ",");
    log_append_view(log, entity->system_identifier);
    log_append(log, ",");
    log_append_view(log, entity->public_identifier);
    log_append(log, ")");
    return SXML_STATUS_OK;
}

static const sxml_tree_builder_operations log_operations = {
    .start_document = log_start_document,
    .end_document = log_end_document,
    .start_element = log_start_element,
    .add_namespace_binding = log_add_namespace_binding,
    .add_attribute = log_add_attribute,
    .end_element = log_end_element,
    .add_text = log_add_text,
    .add_comment = log_add_comment,
    .add_processing_instruction = log_add_processing_instruction,
    .add_unparsed_entity = log_add_unparsed_entity,
};

/* ------------------------------------------------------------ Fixture */

typedef struct parser_fixture {
    sxml_name_pool *name_pool;
    sxml_xml_parser *parser;
} parser_fixture;

static sxml_status fixture_create(parser_fixture *fixture,
                                  sxml_xml_limits limits) {
    *fixture = (parser_fixture){.name_pool = NULL, .parser = NULL};
    sxml_status status = sxml_name_pool_create(
        sxml_default_allocator(),
        &(sxml_name_pool_configuration){.allocator = sxml_default_allocator(),
                                        .maximum_byte_count = 0},
        &fixture->name_pool);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xml_parser_create(sxml_default_allocator(),
                                   &(sxml_xml_parser_configuration){
                                       .allocator = sxml_default_allocator(),
                                       .name_pool = fixture->name_pool,
                                       .limits = limits},
                                   &fixture->parser);
    }
    return status;
}

static void fixture_destroy(parser_fixture *fixture) {
    sxml_xml_parser_destroy(fixture->parser);
    sxml_name_pool_destroy(fixture->name_pool);
}

static const sxml_xml_limits no_limits = {
    .maximum_depth = 0, .maximum_entity_expansion_byte_count = 0};

static sxml_diagnostic empty_diagnostic(void) {
    return (sxml_diagnostic){
        .description = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
}

static sxml_xml_parse_request request_for(sxml_byte_view input,
                                          sxml_diagnostic *diagnostic) {
    return (sxml_xml_parse_request){
        .input = input,
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xml"),
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .encoding_name = {.bytes = NULL, .byte_count = 0},
        .diagnostic = diagnostic};
}

/* One parse into the event log; check mode must agree on the status. */
typedef struct parse_outcome {
    sxml_status status;
    sxml_status check_status;
    sxml_diagnostic diagnostic;
} parse_outcome;

static parse_outcome parse_bytes(const parser_fixture *fixture,
                                 sxml_byte_view input, event_log *log) {
    *log = (event_log){
        .name_pool = fixture->name_pool,
        .text = "",
        .byte_count = 0,
        .element_location = {.source_name = {.bytes = NULL, .byte_count = 0},
                             .line_number = 0,
                             .column_number = 0,
                             .byte_offset = 0}};
    parse_outcome outcome = {.status = SXML_STATUS_OK,
                             .check_status = SXML_STATUS_OK,
                             .diagnostic = empty_diagnostic()};
    sxml_xml_parse_request request = request_for(input, &outcome.diagnostic);
    outcome.status = sxml_xml_parser_parse(
        fixture->parser, &request,
        (sxml_tree_builder){.state = log, .operations = &log_operations});
    sxml_diagnostic check_diagnostic = empty_diagnostic();
    request.diagnostic = &check_diagnostic;
    outcome.check_status = sxml_xml_parser_check(fixture->parser, &request);
    return outcome;
}

static parse_outcome parse_text(const parser_fixture *fixture,
                                const char *document, event_log *log) {
    return parse_bytes(fixture, sxml_byte_view_from_c_string(document), log);
}

/* Parses document and expects the event log expected. */
static bool expect_log(sxml_test_state *state, const char *document,
                       const char *expected) {
    parser_fixture fixture;
    if (fixture_create(&fixture, no_limits) != SXML_STATUS_OK) {
        return false;
    }
    event_log log;
    const parse_outcome outcome = parse_text(&fixture, document, &log);
    const bool passed = outcome.status == SXML_STATUS_OK &&
                        outcome.check_status == SXML_STATUS_OK &&
                        strcmp(log.text, expected) == 0;
    if (!passed) {
        printf("    document: %s\n    expected: %s\n    actual:   %s (%s: "
               "%.*s)\n",
               document, expected, log.text,
               (const char *)sxml_status_name(outcome.status).bytes,
               (int)outcome.diagnostic.description.byte_count,
               (const char *)outcome.diagnostic.description.bytes);
    }
    SXML_TEST_EXPECT(state, passed);
    fixture_destroy(&fixture);
    return passed;
}

/* Parses document and expects status with a description starting with
   description_start. */
typedef struct expected_failure {
    sxml_status status;
    const char *description_start;
} expected_failure;

static void expect_failure_of_bytes(sxml_test_state *state,
                                    sxml_byte_view document,
                                    expected_failure expected) {
    parser_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&fixture, no_limits) == SXML_STATUS_OK);
    event_log log;
    const parse_outcome outcome = parse_bytes(&fixture, document, &log);
    const size_t start_byte_count = strlen(expected.description_start);
    const bool passed =
        outcome.status == expected.status &&
        outcome.check_status == expected.status &&
        outcome.diagnostic.description.byte_count >= start_byte_count &&
        memcmp(outcome.diagnostic.description.bytes, expected.description_start,
               start_byte_count) == 0;
    if (!passed) {
        printf(
            "    document: %.*s\n    expected: %s %s\n    actual:   %s %.*s\n",
            (int)document.byte_count, (const char *)document.bytes,
            (const char *)sxml_status_name(expected.status).bytes,
            expected.description_start,
            (const char *)sxml_status_name(outcome.status).bytes,
            (int)outcome.diagnostic.description.byte_count,
            (const char *)outcome.diagnostic.description.bytes);
    }
    SXML_TEST_EXPECT(state, passed);
    fixture_destroy(&fixture);
}

static void expect_failure(sxml_test_state *state, const char *document,
                           expected_failure expected) {
    expect_failure_of_bytes(state, sxml_byte_view_from_c_string(document),
                            expected);
}

static expected_failure malformed(const char *description_start) {
    return (expected_failure){.status = SXML_STATUS_MALFORMED_INPUT,
                              .description_start = description_start};
}

/* ------------------------------------------------- External entities */

/* An entity resolver over a table of URIs and texts; it logs every
   request as "purpose:absolute-uri;". */
typedef struct entity_table {
    const char *const *uris;
    const char *const *texts;
    size_t entry_count;
    char requests[SXML_TEST_LOG_BYTE_COUNT];
    size_t release_count;
} entity_table;

static sxml_status
resolve_table_entity(void *state,
                     const sxml_xml_external_entity_request *request,
                     sxml_xml_external_entity *entity) {
    entity_table *table = state;
    static const char purposes[] = {'S', 'P', 'G'};
    const size_t used = strlen(table->requests);
    (void)snprintf(table->requests + used, sizeof table->requests - used,
                   "%c:%.*s;", purposes[request->purpose],
                   (int)request->absolute_uri.byte_count,
                   (const char *)request->absolute_uri.bytes);
    for (size_t entry_index = 0; entry_index < table->entry_count;
         entry_index++) {
        if (sxml_byte_view_equals(
                request->absolute_uri,
                sxml_byte_view_from_c_string(table->uris[entry_index]))) {
            entity->is_available = true;
            entity->content =
                sxml_byte_view_from_c_string(table->texts[entry_index]);
        }
    }
    return SXML_STATUS_OK;
}

static void release_table_entity(void *state,
                                 const sxml_xml_external_entity *entity) {
    (void)entity;
    entity_table *table = state;
    table->release_count += 1;
}

static const sxml_xml_entity_resolver_operations table_resolver_operations = {
    .resolve = resolve_table_entity,
    .release = release_table_entity,
};

/* Parses document with base URI http://x/d.xml and table as its entity
   resolver; check mode must agree on the status. */
typedef struct resolved_parse {
    const char *document;
    entity_table *table;
    sxml_xml_limits limits;
} resolved_parse;

static parse_outcome parse_with_entities(const resolved_parse *parse,
                                         event_log *log) {
    parse_outcome outcome = {.status = SXML_STATUS_OUT_OF_MEMORY,
                             .check_status = SXML_STATUS_OUT_OF_MEMORY,
                             .diagnostic = empty_diagnostic()};
    parser_fixture fixture;
    if (fixture_create(&fixture, parse->limits) != SXML_STATUS_OK) {
        return outcome;
    }
    *log = (event_log){
        .name_pool = fixture.name_pool,
        .text = "",
        .byte_count = 0,
        .element_location = {.source_name = {.bytes = NULL, .byte_count = 0},
                             .line_number = 0,
                             .column_number = 0,
                             .byte_offset = 0}};
    sxml_xml_parse_request request = request_for(
        sxml_byte_view_from_c_string(parse->document), &outcome.diagnostic);
    request.base_uri = SXML_BYTE_VIEW_FROM_LITERAL("http://x/d.xml");
    request.entity_resolver = (sxml_xml_entity_resolver){
        .state = parse->table, .operations = &table_resolver_operations};
    outcome.status = sxml_xml_parser_parse(
        fixture.parser, &request,
        (sxml_tree_builder){.state = log, .operations = &log_operations});
    sxml_diagnostic check_diagnostic = empty_diagnostic();
    request.diagnostic = &check_diagnostic;
    outcome.check_status = sxml_xml_parser_check(fixture.parser, &request);
    fixture_destroy(&fixture);
    return outcome;
}

static void test_external_entities_are_read(sxml_test_state *state) {
    static const char *const uris[] = {
        "http://x/d.dtd", "http://x/sub/p.ent", "http://x/sub/g.ent",
        "http://x/c.ent"};
    static const char *const texts[] = {
        /* The external subset: a text declaration, a parameter-entity
           reference inside a declaration (4.4.8), conditional sections
           (3.4), an external parameter entity whose own declarations
           resolve against its URI (4.2.2). */
        "<?xml encoding='UTF-8'?>\n"
        "<!ENTITY % type 'CDATA'>\n"
        "<!ENTITY % on 'INCLUDE'>\n"
        "<!ATTLIST r a %type; 'from-dtd' b CDATA 'dtd'>\n"
        "<![%on;[<!ENTITY i 'included'>]]>\n"
        "<![IGNORE[<!ENTITY i 'ignored'> <![ x ]]> ]]>\n"
        "<!ENTITY % p SYSTEM 'sub/p.ent'>\n"
        "%p;\n",
        "<!ENTITY g SYSTEM 'g.ent'><!ENTITY v \"%type;-value\">",
        "<?xml version='1.0' encoding='UTF-8'?><e>&i;</e>\r\ntail",
        "<c/>"};
    entity_table table = {.uris = uris,
                          .texts = texts,
                          .entry_count = 4,
                          .requests = "",
                          .release_count = 0};
    event_log log;
    /* The internal subset comes first and wins (2.8); the external general
       entity is parsed as [78] extParsedEnt, its line ends normalized
       (2.11). */
    parse_outcome outcome = parse_with_entities(
        &(resolved_parse){
            .document = "<!DOCTYPE r SYSTEM 'd.dtd' [<!ATTLIST r b CDATA 'internal'>]>"
                        "<r>&i;&g;&v;</r>",
            .table = &table,
            .limits = no_limits},
        &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK &&
                                outcome.check_status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(log.text, "D<r @b=internal @a=from-dtd"
                                             "'included'<e'included'</>"
                                             "'\ntail''CDATA-value'</>/D") == 0);
    SXML_TEST_EXPECT(state,
                     strcmp(table.requests,
                            "S:http://x/d.dtd;P:http://x/sub/p.ent;"
                            "G:http://x/sub/g.ent;S:http://x/d.dtd;"
                            "P:http://x/sub/p.ent;G:http://x/sub/g.ent;") == 0);
    SXML_TEST_EXPECT(state, table.release_count == 6);
    if (strcmp(log.text, "") != 0 && outcome.status != SXML_STATUS_OK) {
        printf("    %s\n    %.*s\n", log.text,
               (int)outcome.diagnostic.description.byte_count,
               (const char *)outcome.diagnostic.description.bytes);
    }
    /* An entity the resolver does not deliver is not read (4.4.3, 5.1). */
    table.requests[0] = '\0';
    outcome = parse_with_entities(
        &(resolved_parse){.document = "<!DOCTYPE r [<!ENTITY m SYSTEM 'none'>]>"
                                      "<r>&m;</r>",
                          .table = &table,
                          .limits = no_limits},
        &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK &&
                                strcmp(log.text, "D<r</>/D") == 0 &&
                                strcmp(table.requests,
                                       "G:http://x/none;G:http://x/none;") == 0);
    /* Not-well-formed external text, and recursion through it (4.1). */
    static const char *const bad_uris[] = {"http://x/bad.ent", "http://x/self.ent"};
    static const char *const bad_texts[] = {"<a>", "<!ENTITY % s SYSTEM 'self.ent'>%s;"};
    entity_table bad = {.uris = bad_uris,
                        .texts = bad_texts,
                        .entry_count = 2,
                        .requests = "",
                        .release_count = 0};
    outcome = parse_with_entities(
        &(resolved_parse){.document = "<!DOCTYPE r [<!ENTITY b SYSTEM 'bad.ent'>]>"
                                      "<r>&b;</r>",
                          .table = &bad,
                          .limits = no_limits},
        &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_MALFORMED_INPUT);
    outcome = parse_with_entities(
        &(resolved_parse){.document = "<!DOCTYPE r SYSTEM 'self.ent'><r/>",
                          .table = &bad,
                          .limits = no_limits},
        &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_MALFORMED_INPUT);
    /* "Billion laughs" through parameter entities stops at the bound. */
    static const char *const laugh_uris[] = {"http://x/l.dtd"};
    static const char *const laugh_texts[] = {
        "<!ENTITY % a 'haha haha'><!ENTITY % b '%a;%a;%a;%a;%a;%a;'>"
        "<!ENTITY % c '%b;%b;%b;%b;%b;%b;'><!ELEMENT r (%c;)>"};
    entity_table laughs = {.uris = laugh_uris,
                           .texts = laugh_texts,
                           .entry_count = 1,
                           .requests = "",
                           .release_count = 0};
    outcome = parse_with_entities(
        &(resolved_parse){
            .document = "<!DOCTYPE r SYSTEM 'l.dtd'><r/>",
            .table = &laughs,
            .limits = {.maximum_depth = 0,
                       .maximum_entity_expansion_byte_count = 200}},
        &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED);
}

/* -------------------------------------------------------------- Cases */

static void test_parser_lifecycle(sxml_test_state *state) {
    sxml_name_pool *name_pool = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_name_pool_create(sxml_default_allocator(),
                                     &(sxml_name_pool_configuration){
                                         .allocator = sxml_default_allocator(),
                                         .maximum_byte_count = 0},
                                     &name_pool) == SXML_STATUS_OK);
    sxml_xml_parser_configuration configuration = {.allocator =
                                                       sxml_default_allocator(),
                                                   .name_pool = name_pool,
                                                   .limits = no_limits};
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_xml_parser_memory_requirements(
                          &configuration, &requirements) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count > 0);
    alignas(max_align_t) uint8_t memory[2048];
    SXML_TEST_REQUIRE(state, requirements.byte_count + 16 <= sizeof memory);
    sxml_xml_parser *parser = NULL;
    SXML_TEST_EXPECT(
        state,
        sxml_xml_parser_initialize(
            (sxml_memory_view){.bytes = memory,
                               .byte_count = requirements.byte_count - 1},
            &configuration, &parser) == SXML_STATUS_INSUFFICIENT_MEMORY);
    if (requirements.alignment > 1) {
        SXML_TEST_EXPECT(
            state,
            sxml_xml_parser_initialize(
                (sxml_memory_view){.bytes = memory + 1,
                                   .byte_count = requirements.byte_count},
                &configuration, &parser) == SXML_STATUS_MISALIGNED_MEMORY);
    }
    SXML_TEST_EXPECT(state, parser == NULL);
    SXML_TEST_REQUIRE(
        state,
        sxml_xml_parser_initialize(
            (sxml_memory_view){.bytes = memory, .byte_count = sizeof memory},
            &configuration, &parser) == SXML_STATUS_OK);
    sxml_diagnostic diagnostic = empty_diagnostic();
    const sxml_xml_parse_request request =
        request_for(SXML_BYTE_VIEW_FROM_LITERAL("<a/>"), &diagnostic);
    SXML_TEST_EXPECT(state,
                     sxml_xml_parser_check(parser, &request) == SXML_STATUS_OK);
    sxml_xml_parser_finalize(parser);
    sxml_xml_parser_finalize(NULL);
    sxml_xml_parser_destroy(NULL);
    configuration.name_pool = NULL;
    SXML_TEST_EXPECT(state, sxml_xml_parser_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration.name_pool = name_pool;
    configuration.allocator =
        (sxml_allocator){.state = NULL, .operations = NULL};
    SXML_TEST_EXPECT(state, sxml_xml_parser_create(sxml_default_allocator(),
                                                   &configuration, &parser) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration.allocator = sxml_default_allocator();
    SXML_TEST_REQUIRE(state, sxml_xml_parser_create(sxml_default_allocator(),
                                                    &configuration,
                                                    &parser) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, sxml_xml_parser_parse(
                   parser, &request,
                   (sxml_tree_builder){.state = NULL, .operations = NULL}) ==
                   SXML_STATUS_INVALID_ARGUMENT);
    sxml_xml_parser_destroy(parser);
    sxml_name_pool_destroy(name_pool);
}

static void test_events_follow_the_document(sxml_test_state *state) {
    expect_log(state, "<a/>", "D<a</>/D");
    expect_log(state,
               "<?xml version='1.0'?>\n<!--c--><?p d?>\n"
               "<r xmlns='urn:d' xmlns:p='urn:p' p:a='1' b=\"2\">t<p:e/>"
               "<![CDATA[<x>]]><!--in--><?q  data ?></r><!--after-->",
               "D(!c)(?p d)<{urn:d}r ns(=urn:d) ns(p=urn:p) @{urn:p}a=1 @b=2"
               "'t'<{urn:p}e</>'<x>'(!in)(?q data )</>(!after)/D");
    /* Default namespace undeclared again; the xml prefix is always bound. */
    expect_log(state, "<a xmlns='urn:a'><b xmlns='' xml:lang='en'/></a>",
               "D<{urn:a}a ns(=urn:a)<b ns(=) "
               "@{http://www.w3.org/XML/1998/namespace}lang=en</></>/D");
    /* Predefined entities and character references (4.1, 4.6). */
    expect_log(state,
               "<a x='&lt;&#65;&#x42;'>&amp;&gt;&apos;&quot;&#x1F600;</a>",
               "D<a @x=<AB'&''>'''''\"''\xF0\x9F\x98\x80'</>/D");
}

static void test_line_ends_are_normalized(sxml_test_state *state) {
    /* XML 1.0, section 2.11; a character reference keeps its CR. */
    expect_log(state, "<a>x\r\ny\rz&#13;</a>", "D<a'x''\n''y''\n''z''\r'</>/D");
    expect_log(state, "<a x='1\r\n2\r3\t4&#13;'/>", "D<a @x=1 2 3 4\r</>/D");
    expect_log(state, "<a><!--x\r\ny--><?p a\r\nb?><![CDATA[c\rd]]></a>",
               "D<a(!x\ny)(?p a\nb)'c''\n''d'</>/D");
}

static void test_the_internal_subset_is_processed(sxml_test_state *state) {
    /* Entities, defaults and unparsed entities (4.2, 3.3.2, 4.5). */
    expect_log(
        state,
        "<!DOCTYPE r [\n"
        "<!ENTITY e 'x<b/>y'>\n"
        "<!ENTITY e 'ignored'>\n"
        "<!NOTATION n SYSTEM 'n'>\n"
        "<!ENTITY u SYSTEM 'u.bin' NDATA n>\n"
        "<!ENTITY v PUBLIC '-//p' 'v.bin' NDATA n>\n"
        "<!ATTLIST r d CDATA 'def' t NMTOKENS '  a   b ' i ID #IMPLIED>\n"
        "<!ATTLIST r d CDATA 'second' f CDATA #FIXED 'fixed'>\n"
        "<!ATTLIST b r IDREFS #IMPLIED>\n"
        "]>\n"
        "<r i=' k ' t=' c  d '>&e;<b r='x y'/></r>",
        "DU(u,u.bin,)U(v,v.bin,-//p)<r @i=k!id @t=c d @d=def @f=fixed"
        "'x'<b</>'y'<b @r=x y!refs</></>/D");
    /* A parameter entity between declarations is included (2.8, 4.4.8). */
    expect_log(state,
               "<!DOCTYPE a [<!ENTITY % d \"<!ENTITY e 'v'>\"> %d;]><a>&e;</a>",
               "D<a'v'</>/D");
    /* Nested entities, also in attribute values (4.4.5). */
    expect_log(state,
               "<!DOCTYPE a [<!ENTITY i 'in'><!ENTITY o '[&i;&#38;#65;]'>]>"
               "<a x='&o;'>&o;</a>",
               "D<a @x=[inA]'[''in''A'']'</>/D");
    /* xml:id is an ID (xml:id 1.0, section 4). */
    expect_log(state, "<a xml:id=' x  y '/>",
               "D<a @{http://www.w3.org/XML/1998/namespace}id=x y!id</>/D");
    /* Character references in entity values are expanded when declared,
       and text there is not normalized again. */
    expect_log(state, "<!DOCTYPE a [<!ENTITY e '&#13;&#10;'>]><a>&e;</a>",
               "D<a'\r\n'</>/D");
}

static void
test_unread_parameter_entities_stop_declarations(sxml_test_state *state) {
    /* XML 1.0, section 5.1: after %p; the attribute-list declaration is
       not processed, and undeclared entities are not errors (4.1). */
    expect_log(state,
               "<!DOCTYPE a [<!ENTITY % p SYSTEM 'p.ent'> %p;"
               "<!ATTLIST a x CDATA '1'>]><a>&undeclared;</a>",
               "D<a</>/D");
    expect_log(state, "<!DOCTYPE a SYSTEM 'a.dtd'><a>&undeclared;</a>",
               "D<a</>/D");
    /* standalone="yes" keeps both. */
    expect_failure(state,
                   "<?xml version='1.0' standalone='yes'?>"
                   "<!DOCTYPE a SYSTEM 'a.dtd'><a>&undeclared;</a>",
                   malformed("WFC: Entity Declared"));
    /* External parsed entities are not read in content (4.4.3). */
    expect_log(state, "<!DOCTYPE a [<!ENTITY x SYSTEM 'x.ent'>]><a>&x;</a>",
               "D<a</>/D");
}

static void test_well_formedness_constraints(sxml_test_state *state) {
    expect_failure(state, "<a></b>", malformed("not well-formed"));
    expect_failure(state, "<a x='1' x='2'/>",
                   malformed("WFC: Unique Att Spec"));
    expect_failure(state, "<a>&#0;</a>", malformed("WFC: Legal Character"));
    expect_failure_of_bytes(
        state,
        (sxml_byte_view){.bytes = (const uint8_t *)"<a>\0</a>",
                         .byte_count = 8},
        malformed("U+0000"));
    expect_failure(state, "<a>&#x0;</a>", malformed("WFC: Legal Character"));
    expect_failure(state, "<a>&#xFFFE;</a>", malformed("WFC: Legal Character"));
    expect_failure(state, "<a>&#99999999999999999999;</a>",
                   malformed("WFC: Legal Character"));
    expect_failure(state, "<a x='&#1;'/>", malformed("WFC: Legal Character"));
    expect_failure(state, "<a>&e;</a>", malformed("WFC: Entity Declared"));
    expect_failure(state, "<a x='&e;'/>", malformed("WFC: Entity Declared"));
    expect_failure(
        state, "<!DOCTYPE a [<!ATTLIST a x CDATA '&e;'><!ENTITY e 'v'>]><a/>",
        malformed("WFC: Entity Declared"));
    expect_failure(state,
                   "<!DOCTYPE a [<!NOTATION n SYSTEM 'n'>"
                   "<!ENTITY u SYSTEM 'u' NDATA n>]><a>&u;</a>",
                   malformed("WFC: Parsed Entity"));
    expect_failure(
        state, "<!DOCTYPE a [<!ENTITY e '&f;'><!ENTITY f '&e;'>]><a>&e;</a>",
        malformed("WFC: No Recursion"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY e '&e;'>]><a x='&e;'/>",
                   malformed("WFC: No Recursion"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY % p '%p;'>]><a/>",
                   malformed("not well-formed"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY e '<b>'>]><a>&e;</a>",
                   malformed("the replacement text of an entity referenced"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY e ']]&#62;'>]><a>&e;</a>",
                   malformed("the replacement text of an entity referenced"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY e '</a><a>'>]><a>&e;</a>",
                   malformed("the replacement text of an entity referenced"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY e SYSTEM 'x'>]><a x='&e;'/>",
                   malformed("WFC: No External Entity References"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY e '&#60;'>]><a x='&e;'/>",
                   malformed("WFC: No < in Attribute Values"));
    expect_failure(
        state,
        "<!DOCTYPE a [<!ENTITY e '&#60;'><!ATTLIST a x CDATA '&e;'>]><a/>",
        malformed("WFC: No < in Attribute Values"));
    expect_failure(
        state, "<!DOCTYPE a [<!ENTITY e '&#38;'>]><a x='&e;'/>",
        malformed("the replacement text of an entity in an attribute"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY % p '<!ENTITY'>%p;]><a/>",
                   malformed("WFC: PE Between Declarations"));
    expect_failure(
        state, "<?xml version='1.0' standalone='yes'?><!DOCTYPE a [%p;]><a/>",
        malformed("WFC: Entity Declared"));
}

static void test_namespace_constraints(sxml_test_state *state) {
    expect_failure(state, "<p:a/>", malformed("NSC: Prefix Declared"));
    expect_failure(state, "<a p:x='1'/>", malformed("NSC: Prefix Declared"));
    expect_failure(state, "<a:b:c xmlns:a='u'/>",
                   malformed("NSC: a name must be"));
    expect_failure(state, "<a :b='1'/>", malformed("NSC: a name must be"));
    expect_failure(state, "<a xmlns:p=''/>",
                   malformed("NSC: a namespace declaration"));
    expect_failure(state, "<a xmlns:xml='urn:x'/>", malformed("NSC: Reserved"));
    expect_failure(state, "<a xmlns:x='http://www.w3.org/XML/1998/namespace'/>",
                   malformed("NSC: Reserved"));
    expect_failure(state, "<a xmlns:xmlns='urn:x'/>",
                   malformed("NSC: Reserved"));
    expect_failure(state, "<a xmlns='http://www.w3.org/2000/xmlns/'/>",
                   malformed("NSC: Reserved"));
    expect_failure(state, "<xmlns:a/>", malformed("NSC: element names"));
    expect_failure(state, "<a xmlns:p='u' xmlns:q='u' p:x='1' q:x='2'/>",
                   malformed("NSC: Attributes Unique"));
    expect_failure(state, "<a><?p:i?></a>", malformed("NSC: entity names"));
    expect_failure(state, "<!DOCTYPE a [<!ENTITY p:e 'v'>]><a/>",
                   malformed("NSC: entity names"));
    /* xml may be declared with its own namespace. */
    expect_log(state, "<a xmlns:xml='http://www.w3.org/XML/1998/namespace'/>",
               "D<a</>/D");
    /* Defaults from the DTD can declare namespaces. */
    expect_log(
        state,
        "<!DOCTYPE p:a [<!ATTLIST p:a xmlns:p CDATA #FIXED 'urn:p'>]><p:a/>",
        "D<{urn:p}a ns(p=urn:p)</>/D");
}

static void test_encodings_are_detected(sxml_test_state *state) {
    parser_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&fixture, no_limits) == SXML_STATUS_OK);
    event_log log;
    static const uint8_t utf16_little_endian[] = {
        0xFF, 0xFE, '<',  0,   'a', 0,   '>', 0,   0xE9, 0,   0x3D,
        0xD8, 0x00, 0xDE, '<', 0,   '/', 0,   'a', 0,    '>', 0};
    parse_outcome outcome =
        parse_bytes(&fixture,
                    (sxml_byte_view){.bytes = utf16_little_endian,
                                     .byte_count = sizeof utf16_little_endian},
                    &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, strcmp(log.text, "D<a'\xC3\xA9\xF0\x9F\x98\x80'</>/D") == 0);
    static const uint8_t utf16_big_endian[] = {
        0xFE, 0xFF, 0, '<', 0, '?', 0, 'x', 0, 'm', 0, 'l', 0, ' ', 0, 'v',
        0,    'e',  0, 'r', 0, 's', 0, 'i', 0, 'o', 0, 'n', 0, '=', 0, '"',
        0,    '1',  0, '.', 0, '0', 0, '"', 0, ' ', 0, 'e', 0, 'n', 0, 'c',
        0,    'o',  0, 'd', 0, 'i', 0, 'n', 0, 'g', 0, '=', 0, '"', 0, 'U',
        0,    'T',  0, 'F', 0, '-', 0, '1', 0, '6', 0, '"', 0, '?', 0, '>',
        0,    '<',  0, 'b', 0, '/', 0, '>'};
    outcome =
        parse_bytes(&fixture,
                    (sxml_byte_view){.bytes = utf16_big_endian,
                                     .byte_count = sizeof utf16_big_endian},
                    &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(log.text, "D<b</>/D") == 0);
    static const uint8_t latin1[] =
        "<?xml version='1.0' encoding='iso-8859-1'?>"
        "<a>\xE9</a>";
    outcome = parse_bytes(
        &fixture,
        (sxml_byte_view){.bytes = latin1, .byte_count = sizeof latin1 - 1},
        &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(log.text, "D<a'\xC3\xA9'</>/D") == 0);
    /* The encoding named outside wins (Appendix F.2). */
    static const uint8_t outside[] = "<a>\xE9</a>";
    event_log outside_log = {.name_pool = fixture.name_pool,
                             .text = "",
                             .byte_count = 0,
                             .element_location = log.element_location};
    sxml_diagnostic diagnostic = empty_diagnostic();
    sxml_xml_parse_request request = request_for(
        (sxml_byte_view){.bytes = outside, .byte_count = sizeof outside - 1},
        &diagnostic);
    request.encoding_name = SXML_BYTE_VIEW_FROM_LITERAL("Latin1");
    SXML_TEST_EXPECT(state,
                     sxml_xml_parser_parse(
                         fixture.parser, &request,
                         (sxml_tree_builder){.state = &outside_log,
                                             .operations = &log_operations}) ==
                         SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     strcmp(outside_log.text, "D<a'\xC3\xA9'</>/D") == 0);
    request.encoding_name = SXML_BYTE_VIEW_FROM_LITERAL("KOI8-R");
    SXML_TEST_EXPECT(state, sxml_xml_parser_check(fixture.parser, &request) ==
                                SXML_STATUS_UNSUPPORTED_FEATURE);
    fixture_destroy(&fixture);
    /* What is refused. */
    expect_failure_of_bytes(
        state, SXML_BYTE_VIEW_FROM_LITERAL("<a>\xC3\x28</a>"),
        (expected_failure){.status = SXML_STATUS_INVALID_UTF8,
                           .description_start =
                               "the bytes are not well-formed UTF-8"});
    expect_failure(state,
                   "<?xml version='1.0' encoding='US-ASCII'?><a>\xC3\xA9</a>",
                   malformed("a byte is not US-ASCII"));
    expect_failure(
        state, "<?xml version='1.0' encoding='Shift_JIS'?><a/>",
        (expected_failure){.status = SXML_STATUS_UNSUPPORTED_FEATURE,
                           .description_start = "the document's encoding"});
    expect_failure(
        state, "\xEF\xBB\xBF<?xml version='1.0' encoding='ISO-8859-1'?><a/>",
        malformed("the encoding declaration contradicts"));
    expect_failure(state, "<?xml version='1.0' encoding='UTF-16'?><a/>",
                   malformed("the encoding declaration contradicts"));
    static const uint8_t lone_surrogate[] = {0xFF, 0xFE, '<',  0,    'a', 0,
                                             '>',  0,    0x00, 0xDC, '<', 0,
                                             '/',  0,    'a',  0,    '>', 0};
    expect_failure_of_bytes(
        state,
        (sxml_byte_view){.bytes = lone_surrogate,
                         .byte_count = sizeof lone_surrogate},
        (expected_failure){.status = SXML_STATUS_INVALID_UTF8,
                           .description_start =
                               "the bytes are not well-formed UTF-16"});
    static const uint8_t ucs4[] = {0, 0, 0, '<', 0, 0, 0, 'a'};
    expect_failure_of_bytes(
        state, (sxml_byte_view){.bytes = ucs4, .byte_count = sizeof ucs4},
        (expected_failure){.status = SXML_STATUS_UNSUPPORTED_FEATURE,
                           .description_start = "the document's encoding"});
    /* Versions 1.x other than 1.0 and 1.1 are read by the 1.0 rules (XML
       1.0 Fifth Edition, 2.8). */
    expect_log(state, "<?xml version='1.2'?><a>&#x85;</a>",
               "D<a'\xC2\x85'</>/D");
    expect_log(state, "<?xml version='1.1'?><a/>", "D<a</>/D");
    /* A 1.1 document may undeclare a prefix (Namespaces in XML 1.1,
       section 5). */
    expect_log(state,
               "<?xml version='1.1'?><a xmlns:p='urn:p'><b xmlns:p=''/></a>",
               "D<a ns(p=urn:p)<b ns(p=)</></>/D");
    expect_failure(
        state,
        "<?xml version='1.1'?><a xmlns:p='urn:p'><b xmlns:p=''><p:c/>"
        "</b></a>",
        malformed("NSC: Prefix Declared"));
}

static void test_version_1_1_rules_apply(sxml_test_state *state) {
    /* XML 1.1 Second Edition, 4.1 and 2.2: the C0 controls as character
       references, also in attribute values and entity values; not in XML
       1.0, and U+0000 in neither. */
    expect_log(state, "<?xml version='1.1'?><a x='&#1;'>&#x1F;&#127;</a>",
               "D<a @x=\x01'\x1F''\x7F'</>/D");
    expect_log(state,
               "<?xml version='1.1'?><!DOCTYPE a [<!ENTITY e '&#2;'>]>"
               "<a>&e;</a>",
               "D<a'\x02'</>/D");
    expect_failure(state, "<a>&#1;</a>", malformed("WFC: Legal Character"));
    expect_failure(state, "<?xml version='1.1'?><a>&#0;</a>",
                   malformed("WFC: Legal Character"));
    /* The restricted characters must not appear literally in 1.1 (2.2);
       XML 1.0 allows the C1 controls. */
    expect_log(state, "<a>\xC2\x80</a>", "D<a'\xC2\x80'</>/D");
    expect_failure(state, "<?xml version='1.1'?><a>\xC2\x80</a>",
                   malformed("a control character must be written"));
    expect_failure(state, "<?xml version='1.1'?><a>\x7F</a>",
                   malformed("a control character must be written"));
    /* Line ends (2.11): NEL, CR NEL and LINE SEPARATOR are one LF each;
       CR LINE SEPARATOR is two. In 1.0 they are ordinary characters. */
    expect_log(state,
               "<?xml version='1.1'?><a>x\xC2\x85y\r\xC2\x85z\xE2\x80\xA8"
               "w\r\xE2\x80\xA8v</a>",
               "D<a'x\ny''\n''z\nw\n\nv'</>/D");
    expect_log(state, "<a>x\xC2\x85y</a>", "D<a'x\xC2\x85y'</>/D");
    /* ...also in markup and attribute values, where LF is whitespace. */
    expect_log(state,
               "<?xml version='1.1'?><a\xC2\x85x='1\xE2\x80\xA8"
               "2'\xE2\x80\xA8/>",
               "D<a @x=1 2</>/D");
    /* ...but not in the XML declaration, read before the translation. */
    expect_failure(state, "<?xml version='1.1'\xC2\x85?><a/>",
                   malformed("not well-formed"));
    /* A character reference keeps NEL. */
    expect_log(state, "<?xml version='1.1'?><a>&#x85;</a>",
               "D<a'\xC2\x85'</>/D");
}

static void test_diagnostics_locate_the_failure(sxml_test_state *state) {
    parser_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&fixture, no_limits) == SXML_STATUS_OK);
    event_log log;
    parse_outcome outcome =
        parse_text(&fixture, "<a>\r\n  <b x='1' x='2'/></a>", &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, outcome.diagnostic.location.line_number == 2);
    SXML_TEST_EXPECT(state, outcome.diagnostic.location.column_number == 3);
    SXML_TEST_EXPECT(state, outcome.diagnostic.location.byte_offset == 7);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(outcome.diagnostic.location.source_name,
                                     SXML_BYTE_VIEW_FROM_LITERAL("test.xml")));
    /* Columns count codepoints, byte offsets count input bytes. */
    outcome = parse_text(&fixture, "<a>\xC3\xA9\xC3\xA9<b>&e;</b></a>", &log);
    SXML_TEST_EXPECT(state, outcome.diagnostic.location.line_number == 1);
    SXML_TEST_EXPECT(state, outcome.diagnostic.location.column_number == 9);
    SXML_TEST_EXPECT(state, outcome.diagnostic.location.byte_offset == 10);
    /* Events carry locations too; in an entity, the reference's. */
    outcome = parse_text(
        &fixture, "<!DOCTYPE a [<!ENTITY e '<b/>'>]>\n<a>\n &e;</a>", &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, log.element_location.line_number == 3);
    SXML_TEST_EXPECT(state, log.element_location.column_number == 2);
    fixture_destroy(&fixture);
}

static void test_limits_bound_the_work(sxml_test_state *state) {
    parser_fixture fixture;
    SXML_TEST_REQUIRE(
        state,
        fixture_create(&fixture,
                       (sxml_xml_limits){.maximum_depth = 2,
                                         .maximum_entity_expansion_byte_count =
                                             64}) == SXML_STATUS_OK);
    event_log log;
    SXML_TEST_EXPECT(state, parse_text(&fixture, "<a><b/></a>", &log).status ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     parse_text(&fixture, "<a><b><c/></b></a>", &log).status ==
                         SXML_STATUS_LIMIT_EXCEEDED);
    /* "Billion laughs" (XML 1.0, section 4.4) stops at the bound. */
    const char *laughs =
        "<!DOCTYPE a [<!ENTITY l 'haha'><!ENTITY m '&l;&l;&l;&l;&l;&l;'>"
        "<!ENTITY n '&m;&m;&m;&m;&m;&m;'>]><a x='&n;'>&n;</a>";
    parse_outcome outcome = parse_text(&fixture, laughs, &log);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, outcome.check_status == SXML_STATUS_LIMIT_EXCEEDED);
    fixture_destroy(&fixture);
}

/* Deep documents and long entity chains need no C stack (D81). */
static void test_deep_input_is_walked_iteratively(sxml_test_state *state) {
    /* 30000 levels: far beyond what a recursive walk survives on an 8 MB
       stack under the sanitizers, and linear time since the cursor depth
       is kept (100000 took 6 s under ASan for no more coverage). */
    enum { SXML_TEST_DEPTH = 30000, SXML_TEST_CHAIN = 2000 };
    const sxml_allocator allocator = sxml_default_allocator();
    void *memory = NULL;
    SXML_TEST_REQUIRE(
        state,
        sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){
                .byte_count = (size_t)SXML_TEST_DEPTH * 7 + 64, .alignment = 1},
            &memory) == SXML_STATUS_OK);
    static const char open_tag[] = {'<', 'a', '>'};
    static const char close_tag[] = {'<', '/', 'a', '>'};
    char *text = memory;
    size_t byte_count = 0;
    for (int level = 0; level < SXML_TEST_DEPTH; level++) {
        memcpy(text + byte_count, open_tag, sizeof open_tag);
        byte_count += 3;
    }
    for (int level = 0; level < SXML_TEST_DEPTH; level++) {
        memcpy(text + byte_count, close_tag, sizeof close_tag);
        byte_count += 4;
    }
    parser_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&fixture, no_limits) == SXML_STATUS_OK);
    sxml_diagnostic diagnostic = empty_diagnostic();
    sxml_xml_parse_request request =
        request_for((sxml_byte_view){.bytes = (const uint8_t *)text,
                                     .byte_count = byte_count},
                    &diagnostic);
    SXML_TEST_EXPECT(state, sxml_xml_parser_check(fixture.parser, &request) ==
                                SXML_STATUS_OK);
    sxml_allocator_release(allocator, memory);
    SXML_TEST_REQUIRE(state,
                      sxml_allocator_allocate(
                          allocator,
                          (sxml_allocation_request){
                              .byte_count = (size_t)SXML_TEST_CHAIN * 40 + 64,
                              .alignment = 1},
                          &memory) == SXML_STATUS_OK);
    text = memory;
    byte_count = (size_t)snprintf(text, 64, "<!DOCTYPE a [<!ENTITY e0 'end'>");
    for (int link_index = 1; link_index < SXML_TEST_CHAIN; link_index++) {
        byte_count +=
            (size_t)snprintf(text + byte_count, 40, "<!ENTITY e%d '&e%d;'>",
                             link_index, link_index - 1);
    }
    byte_count += (size_t)snprintf(text + byte_count, 40, "]><a>&e%d;</a>",
                                   SXML_TEST_CHAIN - 1);
    request.input = (sxml_byte_view){.bytes = (const uint8_t *)text,
                                     .byte_count = byte_count};
    event_log log = {.name_pool = fixture.name_pool,
                     .text = "",
                     .byte_count = 0,
                     .element_location = diagnostic.location};
    SXML_TEST_EXPECT(state,
                     sxml_xml_parser_parse(
                         fixture.parser, &request,
                         (sxml_tree_builder){.state = &log,
                                             .operations = &log_operations}) ==
                         SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(log.text, "D<a'end'</>/D") == 0);
    sxml_allocator_release(allocator, memory);
    fixture_destroy(&fixture);
}

/* A document parsed into the arena tree (D67): unparsed entities come
   after start_document and before the content. */
static void test_documents_build_arena_trees(sxml_test_state *state) {
    const sxml_allocator allocator = sxml_default_allocator();
    parser_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&fixture, no_limits) == SXML_STATUS_OK);
    sxml_type_registry *registry = NULL;
    sxml_item_heap *item_heap = NULL;
    SXML_TEST_REQUIRE(state, sxml_type_registry_create(
                                 allocator,
                                 &(sxml_type_registry_configuration){
                                     .allocator = allocator,
                                     .name_pool = fixture.name_pool,
                                     .maximum_registered_type_count = 0},
                                 &registry) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_item_heap_create(allocator,
                                            &(sxml_item_heap_configuration){
                                                .allocator = allocator,
                                                .name_pool = fixture.name_pool,
                                                .type_registry = registry,
                                                .maximum_byte_count = 0},
                                            &item_heap) == SXML_STATUS_OK);
    sxml_arena_tree *arena_tree = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_arena_tree_create(
                   allocator,
                   &(sxml_arena_tree_configuration){
                       .allocator = allocator,
                       .initial_memory = {.bytes = NULL, .byte_count = 0},
                       .item_heap = item_heap,
                       .maximum_node_count = 0,
                       .maximum_depth = 0,
                       .maximum_text_byte_count = 0,
                       .keeps_locations = true,
                       .duplicate_attribute_policy =
                           SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                       .is_destroyed_when_unreferenced = false,
                       .error_record = NULL},
                   &arena_tree) == SXML_STATUS_OK);
    sxml_diagnostic diagnostic = empty_diagnostic();
    sxml_xml_parse_request request =
        request_for(SXML_BYTE_VIEW_FROM_LITERAL(
                        "<!DOCTYPE a [<!NOTATION n SYSTEM 'n'><!ENTITY u "
                        "SYSTEM 'u' NDATA n>]>"
                        "<!--c--><a xmlns:p='urn:p' p:x='1'>t<b/>u</a>"),
                    &diagnostic);
    request.document_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///d.xml");
    SXML_TEST_REQUIRE(state, sxml_xml_parser_parse(
                                 fixture.parser, &request,
                                 sxml_arena_tree_as_tree_builder(arena_tree)) ==
                                 SXML_STATUS_OK);
    sxml_tree *tree = NULL;
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(arena_tree, &tree) ==
                                 SXML_STATUS_OK);
    sxml_tree_description description = {
        .node_count = 0,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .name_pool = NULL};
    SXML_TEST_REQUIRE(state,
                      sxml_tree_describe(tree, &description) == SXML_STATUS_OK);
    /* document, comment, a, @p:x, text, b, text */
    SXML_TEST_EXPECT(state, description.node_count == 7);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                description.document_uri,
                                SXML_BYTE_VIEW_FROM_LITERAL("file:///d.xml")));
    sxml_arena_tree_destroy(arena_tree);
    sxml_item_heap_destroy(item_heap);
    sxml_type_registry_destroy(registry);
    fixture_destroy(&fixture);
}

/* Every allocation may fail: the parse reports it and leaks nothing. */
static void
test_parsing_survives_every_allocation_failure(sxml_test_state *state) {
    const char *document =
        "<!DOCTYPE r [<!ENTITY e 'x<b/>y'><!ENTITY % d \"<!ENTITY f 'v'>\">%d;"
        "<!ATTLIST r d CDATA 'def' t NMTOKENS ' a  b '>]>"
        "<r xmlns:p='urn:p' p:a='&f;' b='&#65;' c='1' g='2' h='3' i='4' j='5' "
        "k='6' l='7' m='8'>t&e;<![CDATA[c]]><!--x\r\ny--><?p d?></r>";
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting = NULL;
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_create(
                sxml_default_allocator(),
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number},
                &counting) == SXML_STATUS_OK);
        const sxml_allocator allocator =
            sxml_counting_allocator_as_allocator(counting);
        sxml_name_pool *name_pool = NULL;
        sxml_xml_parser *parser = NULL;
        sxml_status status = sxml_name_pool_create(
            allocator,
            &(sxml_name_pool_configuration){.allocator = allocator,
                                            .maximum_byte_count = 0},
            &name_pool);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_parser_create(
                allocator,
                &(sxml_xml_parser_configuration){.allocator = allocator,
                                                 .name_pool = name_pool,
                                                 .limits = no_limits},
                &parser);
        }
        event_log log = {.name_pool = name_pool,
                         .text = "",
                         .byte_count = 0,
                         .element_location = {
                             .source_name = {.bytes = NULL, .byte_count = 0},
                             .line_number = 0,
                             .column_number = 0,
                             .byte_offset = 0}};
        if (status == SXML_STATUS_OK) {
            sxml_diagnostic diagnostic = empty_diagnostic();
            const sxml_xml_parse_request request = request_for(
                sxml_byte_view_from_c_string(document), &diagnostic);
            status = sxml_xml_parser_parse(
                parser, &request,
                (sxml_tree_builder){.state = &log,
                                    .operations = &log_operations});
        }
        sxml_xml_parser_destroy(parser);
        sxml_name_pool_destroy(name_pool);
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        sxml_counting_allocator_destroy(counting);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK ||
                                    status == SXML_STATUS_OUT_OF_MEMORY);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, strstr(log.text, "'x'<b</>'y'") != NULL);
            break;
        }
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 1000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 10);
}

/* Empty values of tokenized attributes, the first values of a document,
   so that the value buffer is still empty: found by fuzzing (packet 1.26,
   tests/fuzz/xml_parse_fuzz.c), where collapsing them offset a null
   pointer (XML 1.0, section 3.3.3; xml:id 1.0, section 4). */
static void test_empty_tokenized_values(sxml_test_state *state) {
    expect_log(state,
               "<!DOCTYPE A [<!ATTLIST A att NMTOKENS #IMPLIED>]><A att=\"\"/>",
               "D<A @att=</>/D");
    expect_log(state, "<a xml:id=''/>",
               "D<a @{http://www.w3.org/XML/1998/namespace}id=!id</>/D");
}

/* A position asked after a later one is the position a fresh walk gives
   (P8): every pair of offsets, over line ends of each kind, a byte order
   mark and characters of each UTF-8 length (XML 1.0, section 2.11). */
static void test_positions_move_back(sxml_test_state *state) {
    static const uint8_t text[] = "\xEF\xBB\xBF<a\r\n b='\xC3\xA9'\r"
                                  "c='\xF0\x9F\x98\x80'\n\n>x\xE2\x82\xAC\r\r</a>";
    const sxml_byte_view view = {.bytes = text, .byte_count = sizeof text - 1};
    static const sxml_xml_encoding encodings[] = {
        SXML_XML_ENCODING_UTF8, SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN};
    for (size_t encoding_index = 0; encoding_index < 2; encoding_index++) {
        const sxml_xml_position_tracker_configuration configuration = {
            .text = view,
            .input_encoding = encodings[encoding_index],
            .has_byte_order_mark = true};
        for (size_t first = 0; first <= view.byte_count; first++) {
            for (size_t second = 0; second <= view.byte_count; second++) {
                sxml_xml_position_tracker moved =
                    sxml_xml_position_tracker_start(configuration);
                (void)sxml_xml_position_tracker_locate(&moved, first);
                const sxml_xml_position after_move =
                    sxml_xml_position_tracker_locate(&moved, second);
                sxml_xml_position_tracker fresh =
                    sxml_xml_position_tracker_start(configuration);
                const sxml_xml_position expected =
                    sxml_xml_position_tracker_locate(&fresh, second);
                SXML_TEST_EXPECT(
                    state,
                    after_move.line_number == expected.line_number &&
                        after_move.column_number == expected.column_number &&
                        after_move.input_byte_offset ==
                            expected.input_byte_offset);
                /* The tracker goes on from there as a fresh one does. */
                const sxml_xml_position at_end =
                    sxml_xml_position_tracker_locate(&moved, view.byte_count);
                const sxml_xml_position expected_end =
                    sxml_xml_position_tracker_locate(&fresh, view.byte_count);
                SXML_TEST_EXPECT(
                    state, at_end.line_number == expected_end.line_number &&
                               at_end.column_number ==
                                   expected_end.column_number &&
                               at_end.input_byte_offset ==
                                   expected_end.input_byte_offset);
            }
        }
    }
}

/* Parsing time grows linearly with the document (D87): 10000 elements
   with attributes on lines of their own, about 400 KB, in well under a
   second; when every element's position restarted from the beginning
   (P8) this walked 2 GB and took minutes under the sanitizers. No clock
   is read (WASI has none for the process); a regression shows as a test
   that runs into the ctest timeout. */
static void test_large_documents_parse_in_linear_time(sxml_test_state *state) {
    enum { SXML_TEST_ELEMENT_COUNT = 10000, SXML_TEST_ITEM_BYTE_COUNT = 64 };
    const sxml_allocator allocator = sxml_default_allocator();
    void *memory = NULL;
    const size_t capacity =
        (size_t)SXML_TEST_ELEMENT_COUNT * SXML_TEST_ITEM_BYTE_COUNT + 64;
    SXML_TEST_REQUIRE(
        state, sxml_allocator_allocate(
                   allocator,
                   (sxml_allocation_request){.byte_count = capacity,
                                             .alignment = 1},
                   &memory) == SXML_STATUS_OK);
    char *text = memory;
    size_t byte_count = (size_t)snprintf(text, 64, "<root>\n");
    for (int item_index = 0; item_index < SXML_TEST_ELEMENT_COUNT;
         item_index++) {
        byte_count += (size_t)snprintf(
            text + byte_count, SXML_TEST_ITEM_BYTE_COUNT,
            "<item id='i%d'\n  kind=\"k\">t\xC3\xA9xt %d</item>\n",
            item_index, item_index);
    }
    byte_count += (size_t)snprintf(text + byte_count, 64, "</root>");
    parser_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&fixture, no_limits) == SXML_STATUS_OK);
    sxml_diagnostic diagnostic = empty_diagnostic();
    const sxml_xml_parse_request request =
        request_for((sxml_byte_view){.bytes = (const uint8_t *)text,
                                     .byte_count = byte_count},
                    &diagnostic);
    event_log log = {.name_pool = fixture.name_pool,
                     .text = "",
                     .byte_count = 0,
                     .element_location = diagnostic.location};
    SXML_TEST_EXPECT(state,
                     sxml_xml_parser_parse(
                         fixture.parser, &request,
                         (sxml_tree_builder){.state = &log,
                                             .operations = &log_operations}) ==
                         SXML_STATUS_OK);
    /* The last item starts on line 2 + 2 * (count - 1). */
    SXML_TEST_EXPECT(state, log.element_location.line_number ==
                                2 + 2 * (SXML_TEST_ELEMENT_COUNT - 1));
    SXML_TEST_EXPECT(state, log.element_location.column_number == 1);
    sxml_allocator_release(allocator, memory);
    fixture_destroy(&fixture);
}

static const sxml_test_case xml_test_cases[] = {
    {.name = "parser_lifecycle", .function = test_parser_lifecycle},
    {.name = "events_follow_the_document",
     .function = test_events_follow_the_document},
    {.name = "line_ends_are_normalized",
     .function = test_line_ends_are_normalized},
    {.name = "the_internal_subset_is_processed",
     .function = test_the_internal_subset_is_processed},
    {.name = "unread_parameter_entities_stop_declarations",
     .function = test_unread_parameter_entities_stop_declarations},
    {.name = "well_formedness_constraints",
     .function = test_well_formedness_constraints},
    {.name = "namespace_constraints", .function = test_namespace_constraints},
    {.name = "encodings_are_detected", .function = test_encodings_are_detected},
    {.name = "diagnostics_locate_the_failure",
     .function = test_diagnostics_locate_the_failure},
    {.name = "version_1_1_rules_apply",
     .function = test_version_1_1_rules_apply},
    {.name = "limits_bound_the_work", .function = test_limits_bound_the_work},
    {.name = "external_entities_are_read",
     .function = test_external_entities_are_read},
    {.name = "deep_input_is_walked_iteratively",
     .function = test_deep_input_is_walked_iteratively},
    {.name = "documents_build_arena_trees",
     .function = test_documents_build_arena_trees},
    {.name = "parsing_survives_every_allocation_failure",
     .function = test_parsing_survives_every_allocation_failure},
    {.name = "empty_tokenized_values", .function = test_empty_tokenized_values},
    {.name = "positions_move_back", .function = test_positions_move_back},
    {.name = "large_documents_parse_in_linear_time",
     .function = test_large_documents_parse_in_linear_time},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xml",
            .cases = xml_test_cases,
            .case_count = SXML_TEST_COUNT(xml_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
