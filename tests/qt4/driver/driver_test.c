/*
 * Unit tests of the QT4 driver's modules (suite_path.h, applicability.h,
 * test_list.h, environment.h, catalog.h, resource_table.h), over files held in memory so they
 * run the same natively and under WASI. Imitates tests/memory_test.c:
 * lifecycle, allocation-failure loops (D23).
 */
#include <stdalign.h>
#include <string.h>

#include "applicability.h"
#include "catalog.h"
#include "resource_table.h"
#include "suite_path.h"
#include "sxml/memory.h"
#include "test_list.h"
#include "test_runner.h"

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ------------------------------------------------------------- Fixtures */

/* One file of the in-memory file reader. */
typedef struct memory_file {
    sxml_byte_view path;
    sxml_byte_view contents;
} memory_file;

typedef struct memory_file_table {
    const memory_file *files;
    size_t file_count;
} memory_file_table;

/* The file reader test double: serves the table's files, copied into the
   request's allocator as the interface asks. */
static sxml_status read_memory_file(void *state,
                                    const sxml_cli_file_read_request *request,
                                    sxml_byte_view *contents) {
    const memory_file_table *table = state;
    for (size_t file_index = 0; file_index < table->file_count;
         file_index += 1) {
        const memory_file *file = &table->files[file_index];
        if (!sxml_byte_view_equals(file->path, request->path)) {
            continue;
        }
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate(
            request->allocator,
            (sxml_allocation_request){.byte_count = file->contents.byte_count,
                                      .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(memory, file->contents.bytes, file->contents.byte_count);
        *contents = (sxml_byte_view){.bytes = memory,
                                     .byte_count = file->contents.byte_count};
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_cli_file_reader_operations memory_file_reader_operations = {
    .read_file = read_memory_file,
};

#define CATALOG_NAMESPACE "xmlns='http://www.w3.org/2010/09/qt-fots-catalog'"

static const memory_file suite_files[] = {
    {SXML_BYTE_VIEW_INITIALIZER("suite/catalog.xml"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<catalog " CATALOG_NAMESPACE ">"
         "<environment name='works'>"
         "<source role='.' file='docs/works.xml'/></environment>"
         "<environment name='twice'><source role='.' file='docs/a.xml'/>"
         "</environment>"
         "<test-set name='fn-a' file='fn/a.xml'/>"
         "<test-set name='op-b' file='./op//b.xml'/>"
         "<test-set name='broken' file='fn/broken.xml'/>"
         "<test-set name='missing' file='fn/missing.xml'/>"
         "<test-set name='wrong' file='fn/wrong.xml'/>"
         "</catalog>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/fn/a.xml"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<test-set " CATALOG_NAMESPACE " name='fn-a'>"
         "<environment name='twice'><source role='$d' file='../docs/b.xml'"
         " uri='http://e/b.xml' declared='true'/></environment>"
         "<dependency type='spec' value='XP31+ XQ31+'/>"
         "<test-case name='a-1'><test>1 + 1</test>"
         "<result><assert-eq>2</assert-eq></result></test-case>"
         "<test-case name='a-2'><environment ref='works'/>"
         "<test file='q/a-2.xq'/><result><assert-true/></result></test-case>"
         "<test-case name='a-3'><environment ref='twice'/><test>$d</test>"
         "<result><assert-count>1</assert-count></result></test-case>"
         "<test-case name='a-4'><environment><param name='x' select='1'"
         " as='xs:integer'/><namespace prefix='p' uri='urn:p'/>"
         "<static-base-uri uri='#UNDEFINED'/></environment>"
         "<dependency type='feature' value='staticTyping'/>"
         "<test>$x</test><result><assert-eq>1</assert-eq></result></test-case>"
         "<test-case name='a-5'><environment ref='nowhere'/><test>1</test>"
         "</test-case>"
         "<test-case name='a-6'><dependency type='spec' value='XQ10+'/>"
         "<test><![CDATA[1]]><!--x--><![CDATA[ + 2]]></test></test-case>"
         "<test-case name='a-7'><module uri='urn:m' file='m.xqm'/>"
         "<test>1</test></test-case>"
         "<test-case name='a-8'></test-case>"
         "</test-set>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/op/b.xml"),
     SXML_BYTE_VIEW_INITIALIZER("<test-set " CATALOG_NAMESPACE " name='op-b'>"
                                "<test-case name='b-1'><test>1</test>"
                                "</test-case></test-set>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/fn/broken.xml"),
     SXML_BYTE_VIEW_INITIALIZER("<test-set " CATALOG_NAMESPACE ">\n<a></b>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/fn/wrong.xml"),
     SXML_BYTE_VIEW_INITIALIZER("<catalog " CATALOG_NAMESPACE "/>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/docs/works.xml"),
     SXML_BYTE_VIEW_INITIALIZER("<works/>")},
};

static const memory_file_table suite_table = {
    .files = suite_files,
    .file_count = sizeof suite_files / sizeof suite_files[0],
};

static sxml_cli_file_reader suite_file_reader(void) {
    return (sxml_cli_file_reader){.state = (void *)&suite_table,
                                  .operations = &memory_file_reader_operations};
}

static sxml_qt4_problem empty_problem(void) {
    return (sxml_qt4_problem){
        .description = {.bytes = NULL, .byte_count = 0},
        .subject = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
}

static sxml_qt4_catalog_configuration
suite_configuration(sxml_allocator allocator) {
    return (sxml_qt4_catalog_configuration){.allocator = allocator,
                                            .file_reader = suite_file_reader(),
                                            .catalog_path =
                                                TEXT("suite/catalog.xml")};
}

static const sxml_qt4_target xpath_40 = {.language = SXML_QT4_LANGUAGE_XPATH,
                                         .version = 40};

static sxml_arena *make_arena(void) {
    sxml_arena *arena = NULL;
    if (sxml_arena_create(
            sxml_default_allocator(),
            &(sxml_arena_configuration){
                .block_allocator = sxml_default_allocator(),
                .block_byte_count = 4096,
                .maximum_block_byte_count = 0,
                .initial_memory = {.bytes = NULL, .byte_count = 0}},
            &arena) != SXML_STATUS_OK) {
        return NULL;
    }
    return arena;
}

/* ----------------------------------------------------------- Suite paths */

typedef struct path_case {
    sxml_byte_view containing_file;
    sxml_byte_view reference;
    sxml_byte_view expected;
} path_case;

static void
test_suite_paths_resolve_against_the_containing_file(sxml_test_state *state) {
    static const path_case cases[] = {
        {SXML_BYTE_VIEW_INITIALIZER("catalog.xml"),
         SXML_BYTE_VIEW_INITIALIZER("fn/abs.xml"),
         SXML_BYTE_VIEW_INITIALIZER("fn/abs.xml")},
        {SXML_BYTE_VIEW_INITIALIZER("fn/doc.xml"),
         SXML_BYTE_VIEW_INITIALIZER("../docs/works.xml"),
         SXML_BYTE_VIEW_INITIALIZER("docs/works.xml")},
        {SXML_BYTE_VIEW_INITIALIZER("prod/a/b.xml"),
         SXML_BYTE_VIEW_INITIALIZER("./c/../../d.xml"),
         SXML_BYTE_VIEW_INITIALIZER("prod/d.xml")},
        {SXML_BYTE_VIEW_INITIALIZER("fn/x.xml"),
         SXML_BYTE_VIEW_INITIALIZER("../../up.xml"),
         SXML_BYTE_VIEW_INITIALIZER("../up.xml")},
        {SXML_BYTE_VIEW_INITIALIZER("fn/x.xml"),
         SXML_BYTE_VIEW_INITIALIZER("a//b.xml"),
         SXML_BYTE_VIEW_INITIALIZER("fn/a/b.xml")},
        {SXML_BYTE_VIEW_INITIALIZER("fn/x.xml"),
         SXML_BYTE_VIEW_INITIALIZER("/abs/../y.xml"),
         SXML_BYTE_VIEW_INITIALIZER("/y.xml")},
        {SXML_BYTE_VIEW_INITIALIZER("fn/x.xml"),
         SXML_BYTE_VIEW_INITIALIZER("/../y.xml"),
         SXML_BYTE_VIEW_INITIALIZER("/y.xml")},
        {SXML_BYTE_VIEW_INITIALIZER("x.xml"), SXML_BYTE_VIEW_INITIALIZER(".."),
         SXML_BYTE_VIEW_INITIALIZER("..")},
        {SXML_BYTE_VIEW_INITIALIZER("fn/x.xml"),
         SXML_BYTE_VIEW_INITIALIZER(".."), SXML_BYTE_VIEW_INITIALIZER("")},
        {SXML_BYTE_VIEW_INITIALIZER(""), SXML_BYTE_VIEW_INITIALIZER(""),
         SXML_BYTE_VIEW_INITIALIZER("")},
    };
    sxml_arena *arena = make_arena();
    SXML_TEST_REQUIRE(state, arena != NULL);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_byte_view resolved = {.bytes = NULL, .byte_count = 0};
        SXML_TEST_EXPECT(
            state, sxml_qt4_suite_path_resolve(
                       &(sxml_qt4_suite_path_request){
                           .containing_file = cases[case_index].containing_file,
                           .reference = cases[case_index].reference,
                           .arena = arena},
                       &resolved) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(
            state, sxml_byte_view_equals(resolved, cases[case_index].expected));
    }
    sxml_byte_view joined = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state, sxml_qt4_concatenate(
                                arena,
                                (sxml_qt4_concatenation){.first = TEXT("a/"),
                                                         .second = TEXT("b")},
                                &joined) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(joined, TEXT("a/b")));
    sxml_arena_destroy(arena);
}

/* --------------------------------------------------------- Applicability */

static void
test_spec_tokens_match_language_and_version(sxml_test_state *state) {
    const sxml_qt4_target xquery_40 = {.language = SXML_QT4_LANGUAGE_XQUERY,
                                       .version = 40};
    SXML_TEST_EXPECT(state,
                     sxml_qt4_spec_token_matches(TEXT("XP40"), xpath_40));
    SXML_TEST_EXPECT(state,
                     sxml_qt4_spec_token_matches(TEXT("XP40+"), xpath_40));
    SXML_TEST_EXPECT(state,
                     sxml_qt4_spec_token_matches(TEXT("XP20+"), xpath_40));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_spec_token_matches(TEXT("XP31"), xpath_40));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_spec_token_matches(TEXT("XQ40+"), xpath_40));
    SXML_TEST_EXPECT(state,
                     sxml_qt4_spec_token_matches(TEXT("XQ10+"), xquery_40));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_spec_token_matches(TEXT("XP41+"), xpath_40));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_spec_token_matches(TEXT("XT30+"), xpath_40));
    SXML_TEST_EXPECT(state, !sxml_qt4_spec_token_matches(TEXT("XP"), xpath_40));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_spec_token_matches(TEXT("XP+"), xpath_40));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_spec_token_matches(TEXT("XP4a"), xpath_40));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_spec_token_matches(TEXT("XP12345"), xpath_40));
}

static bool is_supported(sxml_byte_view dependency_type, sxml_byte_view value) {
    return sxml_qt4_dependency_is_supported((sxml_qt4_dependency_value){
        .dependency_type = dependency_type, .value = value});
}

static void
test_dependency_table_lists_what_sxml_supports(sxml_test_state *state) {
    SXML_TEST_EXPECT(
        state, is_supported(TEXT("feature"), TEXT("higherOrderFunctions")));
    SXML_TEST_EXPECT(state,
                     !is_supported(TEXT("feature"), TEXT("staticTyping")));
    /* Schema awareness (D174). */
    SXML_TEST_EXPECT(state,
                     is_supported(TEXT("feature"), TEXT("schemaImport")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("spec"), TEXT("XP40")));
    SXML_TEST_EXPECT(state, is_supported(TEXT("xml-version"), TEXT("1.1 1.0")));
    /* XML 1.1 documents (D195); not XML 1.0 Fourth Edition. */
    SXML_TEST_EXPECT(state, is_supported(TEXT("xml-version"), TEXT("1.1")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("xml-version"), TEXT("1.0:4-")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("unknown-type"), TEXT("en")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("language"), TEXT("")));
    SXML_TEST_EXPECT(state, is_supported(TEXT("language"), TEXT(" en ")));
    /* fn:load-xquery-module is XQuery's (D154). */
    SXML_TEST_EXPECT(state,
                     !is_supported(TEXT("feature"), TEXT("fn-load-xquery-module")));
    SXML_TEST_EXPECT(state, sxml_qt4_dependency_is_supported((sxml_qt4_dependency_value){
                                .dependency_type = TEXT("feature"),
                                .value = TEXT("fn-load-xquery-module"),
                                .runs_xquery = true}));
}

static void
test_applicability_follows_the_rules_in_order(sxml_test_state *state) {
    const sxml_qt4_dependency set_dependencies[] = {
        {.dependency_type = TEXT("spec"),
         .value = TEXT("XQ10+"),
         .requires_satisfaction = true},
        {.dependency_type = TEXT("feature"),
         .value = TEXT("higherOrderFunctions"),
         .requires_satisfaction = true},
    };
    const sxml_qt4_dependency case_specs[] = {
        {.dependency_type = TEXT("spec"),
         .value = TEXT("XP30+ XQ30+"),
         .requires_satisfaction = true},
    };
    const sxml_qt4_dependency inverted[] = {
        {.dependency_type = TEXT("feature"),
         .value = TEXT("higherOrderFunctions"),
         .requires_satisfaction = false},
    };
    sxml_qt4_environment environment = sxml_qt4_environment_absent();
    sxml_qt4_applicability_request request = {
        .target = xpath_40,
        .test_set_dependencies = {.dependencies = set_dependencies,
                                  .dependency_count = 2},
        .test_case_dependencies = {.dependencies = NULL, .dependency_count = 0},
        .environment = &environment};
    sxml_qt4_applicability applicability = {.is_applicable = true};
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, !applicability.is_applicable);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(applicability.reason, TEXT("spec")));
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(applicability.detail, TEXT("XQ10+")));

    /* A test-case spec replaces the test set's. */
    request.test_case_dependencies = (sxml_qt4_dependency_view){
        .dependencies = case_specs, .dependency_count = 1};
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, applicability.is_applicable);

    request.test_case_dependencies = (sxml_qt4_dependency_view){
        .dependencies = inverted, .dependency_count = 1};
    request.test_set_dependencies.dependency_count = 0;
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, !applicability.is_applicable);
    SXML_TEST_EXPECT(state, applicability.is_inverted);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(applicability.reason, TEXT("feature")));

    request.test_case_dependencies.dependency_count = 0;
    /* Resources and collections of sources run (packet 2.16); a
       collection with a query waits for XQuery. */
    const sxml_qt4_resource resource = {
        .uri = TEXT("u"),
        .file_path = TEXT("f"),
        .media_type = {.bytes = NULL, .byte_count = 0},
        .encoding = {.bytes = NULL, .byte_count = 0}};
    environment.resources =
        (sxml_qt4_resource_view){.resources = &resource, .resource_count = 1};
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    const sxml_byte_view query = TEXT("1 to 10");
    const sxml_qt4_collection collection = {
        .uri = TEXT("urn:c"),
        .sources = {.sources = NULL, .source_count = 0},
        .resources = {.resources = NULL, .resource_count = 0},
        .queries = {.texts = &query, .text_count = 1}};
    environment.collections = (sxml_qt4_collection_view){
        .collections = &collection, .collection_count = 1};
    /* Collections with a query run since packet 3.13. */
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    SXML_TEST_EXPECT(state, applicability.is_in_scope);
    environment.collections.collection_count = 0;

    /* The collations of D97 run; others are skipped. */
    const sxml_qt4_collation collations[] = {
        {.uri = TEXT("http://www.w3.org/2005/xpath-functions/collation/"
                     "html-ascii-case-insensitive"),
         .is_default = false},
        {.uri = TEXT("http://www.w3.org/2013/collation/UCA?strength=primary"),
         .is_default = true},
        {.uri = TEXT("http://www.w3.org/2013/collation/UCA?fallback=no"),
         .is_default = false},
        {.uri = TEXT("http://example.com/collation"), .is_default = false},
    };
    for (size_t collation_count = 1; collation_count <= 4;
         collation_count += 1) {
        environment.collations = (sxml_qt4_collation_view){
            .collations = collations, .collation_count = collation_count};
        sxml_qt4_decide_applicability(&request, &applicability);
        SXML_TEST_EXPECT(state,
                         applicability.is_applicable == (collation_count <= 2));
    }
    environment.resources.resource_count = 0;
    const sxml_qt4_collation codepoint = {
        .uri = TEXT("http://www.w3.org/2005/xpath-functions/collation/"
                    "codepoint"),
        .is_default = true};
    environment.collations = (sxml_qt4_collation_view){.collations = &codepoint,
                                                       .collation_count = 1};
    /* Modules run since packet 3.13: the resource table serves them. */
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, applicability.is_applicable);
}

static void test_scope_follows_the_non_goals_of_d95(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_qt4_feature_is_non_goal(TEXT("schemaImport")));
    SXML_TEST_EXPECT(state,
                     sxml_qt4_feature_is_non_goal(TEXT("fullText-ignore")));
    SXML_TEST_EXPECT(state,
                     sxml_qt4_feature_is_non_goal(TEXT("fn-transform-XSLT30")));
    SXML_TEST_EXPECT(
        state, sxml_qt4_feature_is_non_goal(TEXT(" binary expath-file ")));
    SXML_TEST_EXPECT(
        state, !sxml_qt4_feature_is_non_goal(TEXT("higherOrderFunctions")));
    SXML_TEST_EXPECT(state, !sxml_qt4_feature_is_non_goal(
                                TEXT("schemaImport higherOrderFunctions")));
    SXML_TEST_EXPECT(state, !sxml_qt4_feature_is_non_goal(TEXT("")));
    SXML_TEST_EXPECT(state, sxml_qt4_test_set_is_non_goal(TEXT("upd-delete")));
    SXML_TEST_EXPECT(state, sxml_qt4_test_set_is_non_goal(TEXT("fn-transform")));
    SXML_TEST_EXPECT(state,
                     !sxml_qt4_test_set_is_non_goal(TEXT("fn-transform-x")));
    SXML_TEST_EXPECT(state, !sxml_qt4_test_set_is_non_goal(TEXT("ft")));
    SXML_TEST_EXPECT(state, !sxml_qt4_test_set_is_non_goal(TEXT("fn-abs")));

    const sxml_qt4_dependency xquery[] = {
        {.dependency_type = TEXT("spec"),
         .value = TEXT("XQ40+"),
         .requires_satisfaction = true},
    };
    const sxml_qt4_dependency schema[] = {
        {.dependency_type = TEXT("feature"),
         .value = TEXT("schemaImport"),
         .requires_satisfaction = true},
    };
    const sxml_qt4_dependency no_schema[] = {
        {.dependency_type = TEXT("feature"),
         .value = TEXT("schemaImport"),
         .requires_satisfaction = false},
    };
    sxml_qt4_environment environment = sxml_qt4_environment_absent();
    sxml_qt4_applicability_request request = {
        .target = xpath_40,
        .test_set_dependencies = {.dependencies = xquery,
                                  .dependency_count = 1},
        .test_case_dependencies = {.dependencies = NULL, .dependency_count = 0},
        .environment = &environment,
        .test_set_name = TEXT("fn-abs")};
    sxml_qt4_applicability applicability = {.is_applicable = true};
    /* A spec that does not match is outside every denominator. */
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, !applicability.is_in_scope);
    SXML_TEST_EXPECT(state, applicability.non_goal_kind.byte_count == 0);

    request.test_set_dependencies.dependency_count = 0;
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    SXML_TEST_EXPECT(state, applicability.is_in_scope);

    /* A non-goal test set still runs, out of scope. */
    request.test_set_name = TEXT("fn-parse-html");
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    SXML_TEST_EXPECT(state, !applicability.is_in_scope);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(applicability.non_goal_kind,
                                                  TEXT("test-set")));
    request.test_set_name = TEXT("fn-abs");

    request.test_case_dependencies = (sxml_qt4_dependency_view){
        .dependencies = schema, .dependency_count = 1};
    sxml_qt4_decide_applicability(&request, &applicability);
    /* Schema awareness runs (D174), outside the D95 denominators, and is
       counted as schema-aware. */
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    SXML_TEST_EXPECT(state, !applicability.is_in_scope);
    SXML_TEST_EXPECT(state, applicability.is_schema_aware);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(applicability.non_goal,
                                                  TEXT("schemaImport")));

    /* Asking for a non-goal to be absent is in scope; sxml now has this
       one, so the test case does not run. */
    request.test_case_dependencies.dependencies = no_schema;
    sxml_qt4_decide_applicability(&request, &applicability);
    SXML_TEST_EXPECT(state, !applicability.is_applicable);
    SXML_TEST_EXPECT(state, applicability.is_in_scope);
    SXML_TEST_EXPECT(state, !applicability.is_schema_aware);
}

/* ---------------------------------------------------------- Resource table */

/* Resolves kind and uri through resolver; the resource is released. */
typedef struct resolution {
    sxml_byte_view uri;
    sxml_resource_kind resource_kind;
    bool is_default_collection;
} resolution;

typedef struct resolved {
    sxml_resource_form resource_form;
    sxml_byte_view content;
    sxml_byte_view encoding_name;
    int64_t first_integer;
} resolved;

static resolved resolve_through(sxml_resource_resolver resolver,
                                resolution wanted) {
    sxml_resource_request request;
    memset(&request, 0, sizeof request);
    request.resource_kind = wanted.resource_kind;
    request.uri_reference = wanted.uri;
    request.is_default_collection = wanted.is_default_collection;
    sxml_resource resource;
    memset(&resource, 0, sizeof resource);
    resolved answer = {.resource_form = SXML_RESOURCE_FORM_NONE,
                       .content = {.bytes = NULL, .byte_count = 0},
                       .encoding_name = {.bytes = NULL, .byte_count = 0},
                       .first_integer = 0};
    if (resolver.operations->resolve(resolver.state, &request, &resource) !=
        SXML_STATUS_OK) {
        return answer;
    }
    answer.resource_form = resource.resource_form;
    answer.encoding_name = resource.encoding_name;
    if (resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        /* The bytes go back to the resolver below: compare them first. */
        answer.content = sxml_byte_view_equals(resource.content,
                                               TEXT("<works/>"))
                             ? TEXT("<works/>")
                             : TEXT("?");
    }
    if (resource.resource_form == SXML_RESOURCE_FORM_SEQUENCE) {
        sxml_item item;
        memset(&item, 0, sizeof item);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof datum);
        if (sxml_sequence_get_item(resource.items,
                                   (sxml_item_request){.item_index = 0,
                                                       .error_record = NULL},
                                   &item) == SXML_STATUS_OK &&
            sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK) {
            answer.first_integer = datum.datum_details.integer;
        }
        /* The resolver handed over a reference (processor.h). */
        sxml_sequence_release(resource.items);
    }
    resolver.operations->release(resolver.state, &resource);
    return answer;
}

static void
test_resource_table_serves_resources_and_collections(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_counting_allocator_create(
                   sxml_default_allocator(),
                   &(sxml_counting_allocator_configuration){
                       .backing_allocator = sxml_default_allocator(),
                       .failing_allocation_number = 0},
                   &counting_allocator) == SXML_STATUS_OK);
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(counting_allocator);
    sxml_qt4_catalog *catalog = NULL;
    const sxml_qt4_catalog_configuration configuration =
        suite_configuration(allocator);
    SXML_TEST_REQUIRE(state, sxml_qt4_catalog_create(allocator, &configuration,
                                                     &catalog) ==
                                 SXML_STATUS_OK);
    const sxml_qt4_served_resource resources[] = {
        {.uri = TEXT("http://e/works.txt"),
         .file_path = TEXT("docs/works.xml"),
         .encoding = TEXT("utf-8")},
        {.uri = TEXT("http://e/missing.txt"),
         .file_path = TEXT("docs/missing.txt"),
         .encoding = {.bytes = NULL, .byte_count = 0}},
    };
    const sxml_qt4_served_collection collections[] = {
        {.is_default = false,
         .uri = TEXT("urn:c"),
         .items = sxml_sequence_from_item(sxml_atomic_item_from_integer(1)),
         .uris = sxml_sequence_from_item(sxml_atomic_item_from_integer(2))},
        {.is_default = true,
         .uri = {.bytes = NULL, .byte_count = 0},
         .items = sxml_sequence_from_item(sxml_atomic_item_from_integer(3)),
         .uris = sxml_sequence_from_item(sxml_atomic_item_from_integer(4))},
    };
    const sxml_qt4_resource_table table = {
        .catalog = catalog,
        .allocator = allocator,
        .served_resources = {.served_resources = resources,
                             .served_resource_count = 2},
        .served_collections = {.served_collections = collections,
                               .served_collection_count = 2}};
    const sxml_resource_resolver resolver =
        sxml_qt4_resource_table_resolver(&table);

    resolved answer = resolve_through(
        resolver, (resolution){.resource_kind = SXML_RESOURCE_KIND_TEXT,
                               .uri = TEXT("http://e/works.txt"),
                               .is_default_collection = false});
    SXML_TEST_EXPECT(state, answer.resource_form == SXML_RESOURCE_FORM_BYTES);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(answer.content, TEXT("<works/>")));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(answer.encoding_name, TEXT("utf-8")));
    /* A binary resource has no encoding. */
    answer = resolve_through(
        resolver, (resolution){.resource_kind = SXML_RESOURCE_KIND_BINARY,
                               .uri = TEXT("http://e/works.txt"),
                               .is_default_collection = false});
    SXML_TEST_EXPECT(state, answer.resource_form == SXML_RESOURCE_FORM_BYTES);
    SXML_TEST_EXPECT(state, answer.encoding_name.byte_count == 0);
    /* Suite files by file:///SUITE-PATH, for documents and texts. */
    answer = resolve_through(
        resolver, (resolution){.resource_kind = SXML_RESOURCE_KIND_TEXT,
                               .uri = TEXT("file:///docs/works.xml"),
                               .is_default_collection = false});
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(answer.content, TEXT("<works/>")));
    answer = resolve_through(
        resolver, (resolution){.resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT,
                               .uri = TEXT("file:///docs/works.xml"),
                               .is_default_collection = false});
    SXML_TEST_EXPECT(state, answer.resource_form == SXML_RESOURCE_FORM_BYTES);
    /* Resources are not documents; missing files and other URIs are not
       available. */
    const resolution unavailable[] = {
        {TEXT("http://e/works.txt"), SXML_RESOURCE_KIND_XML_DOCUMENT, false},
        {TEXT("http://e/missing.txt"), SXML_RESOURCE_KIND_TEXT, false},
        {TEXT("http://e/other.txt"), SXML_RESOURCE_KIND_TEXT, false},
        {TEXT("file:///docs/none.txt"), SXML_RESOURCE_KIND_TEXT, false},
        {TEXT("urn:other"), SXML_RESOURCE_KIND_COLLECTION, false},
        {TEXT("urn:c"), SXML_RESOURCE_KIND_XQUERY_MODULE, false},
    };
    for (size_t unavailable_index = 0;
         unavailable_index < sizeof unavailable / sizeof unavailable[0];
         unavailable_index += 1) {
        answer = resolve_through(resolver, unavailable[unavailable_index]);
        SXML_TEST_EXPECT(state, answer.resource_form == SXML_RESOURCE_FORM_NONE);
    }
    const struct {
        resolution wanted;
        int64_t first_integer;
    } collection_cases[] = {
        {{TEXT("urn:c"), SXML_RESOURCE_KIND_COLLECTION, false}, 1},
        {{TEXT("urn:c"), SXML_RESOURCE_KIND_URI_COLLECTION, false}, 2},
        {{TEXT(""), SXML_RESOURCE_KIND_COLLECTION, true}, 3},
        {{TEXT(""), SXML_RESOURCE_KIND_URI_COLLECTION, true}, 4},
    };
    for (size_t case_index = 0;
         case_index < sizeof collection_cases / sizeof collection_cases[0];
         case_index += 1) {
        answer = resolve_through(resolver, collection_cases[case_index].wanted);
        SXML_TEST_EXPECT(state,
                         answer.resource_form == SXML_RESOURCE_FORM_SEQUENCE);
        SXML_TEST_EXPECT(state, answer.first_integer ==
                                    collection_cases[case_index].first_integer);
    }
    sxml_qt4_catalog_destroy(catalog);
    const sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(counting_allocator);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

/* The file URI of the module the table serves for an import of
   namespace_uri with one location hint (empty for none); empty when it
   serves none. */
typedef struct module_import {
    sxml_byte_view namespace_uri;
    sxml_byte_view hint;
    sxml_byte_view absolute_hint;
} module_import;

static sxml_byte_view import_through(sxml_resource_resolver resolver,
                                     module_import import) {
    sxml_resource_request request;
    memset(&request, 0, sizeof request);
    request.resource_kind = SXML_RESOURCE_KIND_XQUERY_MODULE;
    request.uri_reference = import.hint;
    request.absolute_uri = import.absolute_hint;
    request.target_namespace_uri = import.namespace_uri;
    sxml_resource resource;
    memset(&resource, 0, sizeof resource);
    sxml_byte_view served = {.bytes = NULL, .byte_count = 0};
    if (resolver.operations->resolve(resolver.state, &request, &resource) ==
            SXML_STATUS_OK &&
        resource.resource_form == SXML_RESOURCE_FORM_BYTES &&
        sxml_byte_view_equals(resource.content, TEXT("<works/>"))) {
        served = resource.resource_uri;
    }
    resolver.operations->release(resolver.state, &resource);
    return served;
}

/* Packet 3.13: <module> files by target namespace, picked by location
   hint, then by file name, then the first. */
static void test_resource_table_serves_modules(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_counting_allocator_create(
                   sxml_default_allocator(),
                   &(sxml_counting_allocator_configuration){
                       .backing_allocator = sxml_default_allocator(),
                       .failing_allocation_number = 0},
                   &counting_allocator) == SXML_STATUS_OK);
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(counting_allocator);
    sxml_qt4_catalog *catalog = NULL;
    const sxml_qt4_catalog_configuration configuration =
        suite_configuration(allocator);
    SXML_TEST_REQUIRE(state, sxml_qt4_catalog_create(allocator, &configuration,
                                                     &catalog) ==
                                 SXML_STATUS_OK);
    /* Every module file is docs/works.xml; the URIs tell them apart. */
    const sxml_qt4_served_module modules[] = {
        {.namespace_uri = TEXT("urn:m"),
         .location = {.bytes = NULL, .byte_count = 0},
         .file_path = TEXT("docs/works.xml"),
         .file_uri = TEXT("file:///first")},
        {.namespace_uri = TEXT("urn:m"),
         .location = {.bytes = NULL, .byte_count = 0},
         .file_path = TEXT("lib/other.xml"),
         .file_uri = TEXT("file:///by-name")},
        {.namespace_uri = TEXT("urn:m"),
         .location = TEXT("http://e/m.xq"),
         .file_path = TEXT("docs/works.xml"),
         .file_uri = TEXT("file:///by-location")},
    };
    const sxml_qt4_resource_table table = {
        .catalog = catalog,
        .allocator = allocator,
        .served_resources = {.served_resources = NULL,
                             .served_resource_count = 0},
        .served_collections = {.served_collections = NULL,
                               .served_collection_count = 0},
        .served_modules = {.served_modules = modules,
                           .served_module_count = 3}};
    const sxml_resource_resolver resolver =
        sxml_qt4_resource_table_resolver(&table);
    const sxml_byte_view none = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(
        state,
        sxml_byte_view_equals(
            import_through(resolver, (module_import){.namespace_uri = TEXT("urn:m"),
                                                     .hint = TEXT("http://e/m.xq"),
                                                     .absolute_hint = none}),
            TEXT("file:///by-location")));
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(
                   import_through(resolver,
                                  (module_import){
                                      .namespace_uri = TEXT("urn:m"),
                                      .hint = TEXT("m.xq"),
                                      .absolute_hint = TEXT("http://e/m.xq")}),
                   TEXT("file:///by-location")));
    /* lib/other.xml cannot be read: the chosen module is not available. */
    SXML_TEST_EXPECT(
        state,
        import_through(resolver, (module_import){.namespace_uri = TEXT("urn:m"),
                                                 .hint = TEXT("x/other.xml"),
                                                 .absolute_hint = none})
                .byte_count == 0);
    SXML_TEST_EXPECT(
        state,
        sxml_byte_view_equals(
            import_through(resolver, (module_import){.namespace_uri = TEXT("urn:m"),
                                                     .hint = none,
                                                     .absolute_hint = none}),
            TEXT("file:///first")));
    SXML_TEST_EXPECT(
        state,
        import_through(resolver, (module_import){.namespace_uri = TEXT("urn:other"),
                                                 .hint = none,
                                                 .absolute_hint = none})
                .byte_count == 0);
    sxml_qt4_catalog_destroy(catalog);
    const sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(counting_allocator);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

/* ------------------------------------------------------------- Test lists */

static void
test_test_lists_are_read_like_the_check_script(sxml_test_state *state) {
    sxml_arena *arena = make_arena();
    SXML_TEST_REQUIRE(state, arena != NULL);
    sxml_qt4_problem problem = empty_problem();
    sxml_qt4_test_list test_list = {.entries = NULL, .entry_count = 0};
    SXML_TEST_EXPECT(
        state,
        sxml_qt4_test_list_parse(
            &(sxml_qt4_test_list_request){
                .text = TEXT("# comment\n\nfn-a a-1  # why\r\n"
                             "\top-b\tb-1\n   # indented comment\nfn-a a-2"),
                .source_name = TEXT("list.txt"),
                .arena = arena,
                .problem = &problem},
            &test_list) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, test_list.entry_count == 3);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_list.entries[0].test_set_name,
                                           TEXT("fn-a")));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_list.entries[0].test_case_name,
                                           TEXT("a-1")));
    SXML_TEST_EXPECT(state, test_list.entries[0].line_number == 3);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_list.entries[1].test_case_name,
                                           TEXT("b-1")));
    SXML_TEST_EXPECT(state, test_list.entries[1].line_number == 4);
    SXML_TEST_EXPECT(state, test_list.entries[2].line_number == 6);

    const sxml_qt4_test_list before = test_list;
    SXML_TEST_EXPECT(state, sxml_qt4_test_list_parse(
                                &(sxml_qt4_test_list_request){
                                    .text = TEXT("fn-a a-1\nfn-a a-2 extra\n"),
                                    .source_name = TEXT("list.txt"),
                                    .arena = arena,
                                    .problem = &problem},
                                &test_list) == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, test_list.entries == before.entries);
    SXML_TEST_EXPECT(state, problem.location.line_number == 2);
    SXML_TEST_EXPECT(state, sxml_qt4_test_list_parse(
                                &(sxml_qt4_test_list_request){
                                    .text = TEXT("lonely\n"),
                                    .source_name = TEXT("list.txt"),
                                    .arena = arena,
                                    .problem = NULL},
                                &test_list) == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, sxml_qt4_test_list_parse(
                                &(sxml_qt4_test_list_request){
                                    .text = TEXT(""),
                                    .source_name = TEXT("list.txt"),
                                    .arena = arena,
                                    .problem = NULL},
                                &test_list) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_list.entry_count == 0);
    sxml_arena_destroy(arena);
}

/* --------------------------------------------------------------- Catalog */

static sxml_status read_case(sxml_qt4_catalog *catalog, sxml_byte_view name,
                             sxml_qt4_test_case *test_case) {
    size_t test_case_index = 0;
    if (!sxml_qt4_catalog_find_test_case(catalog, name, &test_case_index)) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    return sxml_qt4_catalog_read_test_case(
        catalog,
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = NULL},
        test_case);
}

static sxml_status open_set(sxml_qt4_catalog *catalog, sxml_byte_view name,
                            sxml_qt4_problem *problem) {
    size_t test_set_index = 0;
    if (!sxml_qt4_catalog_find_test_set(catalog, name, &test_set_index)) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    return sxml_qt4_catalog_open_test_set(
        catalog, &(sxml_qt4_test_set_request){.test_set_index = test_set_index,
                                              .problem = problem});
}

static void
test_catalog_resolves_test_cases_and_environments(sxml_test_state *state) {
    sxml_qt4_catalog *catalog = NULL;
    const sxml_qt4_catalog_configuration configuration =
        suite_configuration(sxml_default_allocator());
    SXML_TEST_REQUIRE(
        state, sxml_qt4_catalog_create(sxml_default_allocator(), &configuration,
                                       &catalog) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_test_set_count(catalog) == 0);
    SXML_TEST_EXPECT(state,
                     sxml_qt4_catalog_load(catalog, NULL) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_load(catalog, NULL) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_test_set_count(catalog) == 5);
    sxml_qt4_test_set_entry entry = {
        .name = {.bytes = NULL, .byte_count = 0},
        .file_path = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_get_test_set_entry(
                                catalog, 1, &entry) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(entry.file_path, TEXT("op/b.xml")));
    SXML_TEST_EXPECT(state,
                     sxml_qt4_catalog_get_test_set_entry(catalog, 5, &entry) ==
                         SXML_STATUS_INDEX_OUT_OF_RANGE);
    size_t test_set_index = 0;
    SXML_TEST_EXPECT(state, !sxml_qt4_catalog_find_test_set(
                                catalog, TEXT("nope"), &test_set_index));
    SXML_TEST_EXPECT(
        state, !sxml_qt4_catalog_get_open_test_set(catalog, &test_set_index));
    SXML_TEST_EXPECT(state,
                     sxml_qt4_catalog_get_test_set_document(catalog) == NULL);
    sxml_qt4_test_case test_case = {.name = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_read_test_case(
                                catalog,
                                &(sxml_qt4_test_case_request){
                                    .test_case_index = 0, .problem = NULL},
                                &test_case) == SXML_STATUS_INVALID_ARGUMENT);

    SXML_TEST_REQUIRE(state,
                      open_set(catalog, TEXT("fn-a"), NULL) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_get_open_test_set(catalog, &test_set_index) &&
                   test_set_index == 0);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_test_case_count(catalog) == 8);
    SXML_TEST_EXPECT(state,
                     sxml_qt4_catalog_get_test_set_document(catalog) != NULL);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-1"), &test_case) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(test_case.query_text, TEXT("1 + 1")));
    SXML_TEST_EXPECT(state, test_case.environment.origin ==
                                SXML_QT4_ENVIRONMENT_ORIGIN_ABSENT);
    SXML_TEST_EXPECT(state, test_case.has_result);
    SXML_TEST_EXPECT(state,
                     test_case.test_set_dependencies.dependency_count == 1);
    sxml_qt4_applicability applicability = {.is_applicable = false};
    sxml_qt4_test_case_decide_applicability(
        (sxml_qt4_test_case_target){.test_case = &test_case,
                                    .target = xpath_40},
        &applicability);
    SXML_TEST_EXPECT(state, applicability.is_applicable);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-2"), &test_case) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(test_case.query_file_path,
                                                  TEXT("fn/q/a-2.xq")));
    SXML_TEST_EXPECT(state, test_case.query_text.byte_count == 0);
    SXML_TEST_EXPECT(state, test_case.environment.origin ==
                                SXML_QT4_ENVIRONMENT_ORIGIN_CATALOG);
    SXML_TEST_REQUIRE(state, test_case.environment.sources.source_count == 1);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         test_case.environment.sources.sources[0].file_path,
                         TEXT("docs/works.xml")));
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(
                   test_case.environment.sources.sources[0].role, TEXT(".")));

    /* A local environment wins over a global one of the same name. */
    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-3"), &test_case) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_case.environment.origin ==
                                SXML_QT4_ENVIRONMENT_ORIGIN_TEST_SET);
    SXML_TEST_REQUIRE(state, test_case.environment.sources.source_count == 1);
    const sxml_qt4_source *source = &test_case.environment.sources.sources[0];
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(source->file_path, TEXT("docs/b.xml")));
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(source->uri, TEXT("http://e/b.xml")));
    SXML_TEST_EXPECT(state, source->is_declared);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-4"), &test_case) ==
                                 SXML_STATUS_OK);
    const sxml_qt4_environment *environment = &test_case.environment;
    SXML_TEST_EXPECT(state,
                     environment->origin == SXML_QT4_ENVIRONMENT_ORIGIN_INLINE);
    SXML_TEST_REQUIRE(state, environment->parameters.parameter_count == 1);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         environment->parameters.parameters[0].declared_type,
                         TEXT("xs:integer")));
    SXML_TEST_REQUIRE(
        state,
        environment->namespace_declarations.namespace_declaration_count == 1);
    SXML_TEST_EXPECT(state, environment->static_base_uri_kind ==
                                SXML_QT4_STATIC_BASE_URI_ABSENT);
    sxml_qt4_test_case_decide_applicability(
        (sxml_qt4_test_case_target){.test_case = &test_case,
                                    .target = xpath_40},
        &applicability);
    SXML_TEST_EXPECT(state, !applicability.is_applicable);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(applicability.detail,
                                                  TEXT("staticTyping")));

    sxml_qt4_problem problem = empty_problem();
    size_t test_case_index = 0;
    SXML_TEST_REQUIRE(state, sxml_qt4_catalog_find_test_case(
                                 catalog, TEXT("a-5"), &test_case_index));
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_read_test_case(
                   catalog,
                   &(sxml_qt4_test_case_request){
                       .test_case_index = test_case_index, .problem = &problem},
                   &test_case) == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(problem.subject, TEXT("nowhere")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(problem.location.source_name,
                                                  TEXT("fn/a.xml")));

    /* Text split by a comment is joined; the test-case spec replaces the
       set's, so XP40 does not apply. */
    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-6"), &test_case) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(test_case.query_text, TEXT("1 + 2")));
    SXML_TEST_EXPECT(state, !test_case.has_result);
    sxml_qt4_test_case_decide_applicability(
        (sxml_qt4_test_case_target){.test_case = &test_case,
                                    .target = xpath_40},
        &applicability);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(applicability.detail, TEXT("XQ10+")));

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-7"), &test_case) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, test_case.modules.module_count == 1);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(test_case.modules.modules[0].file_path,
                                     TEXT("fn/m.xqm")));

    SXML_TEST_EXPECT(state, sxml_qt4_catalog_find_test_case(
                                catalog, TEXT("a-8"), &test_case_index));
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_read_test_case(
                   catalog,
                   &(sxml_qt4_test_case_request){
                       .test_case_index = test_case_index, .problem = &problem},
                   &test_case) == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_read_test_case(
                                catalog,
                                &(sxml_qt4_test_case_request){
                                    .test_case_index = 8, .problem = NULL},
                                &test_case) == SXML_STATUS_INDEX_OUT_OF_RANGE);

    SXML_TEST_EXPECT(state,
                     open_set(catalog, TEXT("op-b"), NULL) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_test_case_count(catalog) == 1);
    SXML_TEST_EXPECT(state, !sxml_qt4_catalog_find_test_case(
                                catalog, TEXT("a-1"), &test_case_index));

    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_read_file(
                                catalog,
                                &(sxml_qt4_file_request){
                                    .suite_path = TEXT("docs/works.xml"),
                                    .allocator = sxml_default_allocator()},
                                &contents) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(contents, TEXT("<works/>")));
    sxml_allocator_release(sxml_default_allocator(), (void *)contents.bytes);
    sxml_qt4_catalog_destroy(catalog);
}

static void
test_catalog_reports_broken_and_missing_files(sxml_test_state *state) {
    sxml_qt4_catalog *catalog = NULL;
    const sxml_qt4_catalog_configuration configuration =
        suite_configuration(sxml_default_allocator());
    SXML_TEST_REQUIRE(
        state, sxml_qt4_catalog_create(sxml_default_allocator(), &configuration,
                                       &catalog) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_qt4_catalog_load(catalog, NULL) == SXML_STATUS_OK);
    sxml_qt4_problem problem = empty_problem();
    SXML_TEST_EXPECT(state, open_set(catalog, TEXT("broken"), &problem) ==
                                SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, problem.location.line_number == 2);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(problem.location.source_name,
                                                  TEXT("fn/broken.xml")));
    size_t test_set_index = 0;
    SXML_TEST_EXPECT(
        state, !sxml_qt4_catalog_get_open_test_set(catalog, &test_set_index));
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_test_case_count(catalog) == 0);
    problem = empty_problem();
    SXML_TEST_EXPECT(state, open_set(catalog, TEXT("missing"), &problem) ==
                                SXML_STATUS_INPUT_OUTPUT_FAILED);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                problem.subject, TEXT("suite/fn/missing.xml")));
    SXML_TEST_EXPECT(state, open_set(catalog, TEXT("wrong"), &problem) ==
                                SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(problem.subject, TEXT("test-set")));
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_open_test_set(
                   catalog, &(sxml_qt4_test_set_request){.test_set_index = 5,
                                                         .problem = NULL}) ==
                   SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_qt4_catalog_destroy(catalog);

    /* A missing catalog, and a catalog whose document element is wrong. */
    sxml_qt4_catalog_configuration elsewhere = configuration;
    elsewhere.catalog_path = TEXT("nowhere/catalog.xml");
    SXML_TEST_REQUIRE(state, sxml_qt4_catalog_create(sxml_default_allocator(),
                                                     &elsewhere, &catalog) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_load(catalog, &problem) ==
                                SXML_STATUS_INPUT_OUTPUT_FAILED);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(problem.subject,
                                                  TEXT("nowhere/catalog.xml")));
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_test_set_count(catalog) == 0);
    sxml_qt4_catalog_destroy(catalog);
    elsewhere.catalog_path = TEXT("suite/fn/a.xml");
    SXML_TEST_REQUIRE(state, sxml_qt4_catalog_create(sxml_default_allocator(),
                                                     &elsewhere, &catalog) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_qt4_catalog_load(catalog, &problem) ==
                                SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(problem.subject, TEXT("catalog")));
    sxml_qt4_catalog_destroy(catalog);
}

static void test_catalog_lifecycle_on_caller_memory(sxml_test_state *state) {
    const sxml_qt4_catalog_configuration configuration =
        suite_configuration(sxml_default_allocator());
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_qt4_catalog_memory_requirements(
                          &configuration, &requirements) == SXML_STATUS_OK);
    sxml_qt4_catalog_configuration invalid = configuration;
    invalid.catalog_path = TEXT("");
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_memory_requirements(&invalid, &requirements) ==
                   SXML_STATUS_INVALID_ARGUMENT);
    invalid = configuration;
    invalid.allocator = (sxml_allocator){.state = NULL, .operations = NULL};
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_memory_requirements(&invalid, &requirements) ==
                   SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_memory_requirements(NULL, &requirements) ==
                   SXML_STATUS_INVALID_ARGUMENT);

    static alignas(max_align_t) uint8_t memory[4096];
    SXML_TEST_REQUIRE(state, requirements.byte_count + 1 <= sizeof memory);
    sxml_qt4_catalog *catalog = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_qt4_catalog_initialize(
                         (sxml_memory_view){.bytes = memory, .byte_count = 8},
                         &configuration,
                         &catalog) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(
        state, sxml_qt4_catalog_initialize(
                   (sxml_memory_view){.bytes = memory + 1,
                                      .byte_count = requirements.byte_count},
                   &configuration, &catalog) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, catalog == NULL);
    SXML_TEST_REQUIRE(
        state, sxml_qt4_catalog_initialize(
                   (sxml_memory_view){.bytes = memory,
                                      .byte_count = requirements.byte_count},
                   &configuration, &catalog) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_qt4_catalog_load(catalog, NULL) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     open_set(catalog, TEXT("op-b"), NULL) == SXML_STATUS_OK);
    sxml_qt4_catalog_finalize(catalog);
    sxml_qt4_catalog_finalize(NULL);
    sxml_qt4_catalog_destroy(NULL);
}

/* Loads the catalog, opens every test set and reads every test case,
   through allocator; returns the first failure that is not the suite's
   own (a malformed or missing file). */
static sxml_status walk_the_suite(sxml_allocator allocator) {
    sxml_qt4_catalog *catalog = NULL;
    const sxml_qt4_catalog_configuration configuration =
        suite_configuration(allocator);
    sxml_status status =
        sxml_qt4_catalog_create(allocator, &configuration, &catalog);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_catalog_load(catalog, NULL);
    }
    const size_t test_set_count =
        status == SXML_STATUS_OK ? sxml_qt4_catalog_test_set_count(catalog) : 0;
    for (size_t test_set_index = 0;
         status == SXML_STATUS_OK && test_set_index < test_set_count;
         test_set_index += 1) {
        status = sxml_qt4_catalog_open_test_set(
            catalog, &(sxml_qt4_test_set_request){
                         .test_set_index = test_set_index, .problem = NULL});
        if (status == SXML_STATUS_MALFORMED_INPUT ||
            status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
            status = SXML_STATUS_OK;
            continue;
        }
        const size_t test_case_count =
            status == SXML_STATUS_OK ? sxml_qt4_catalog_test_case_count(catalog)
                                     : 0;
        for (size_t test_case_index = 0;
             status == SXML_STATUS_OK && test_case_index < test_case_count;
             test_case_index += 1) {
            sxml_qt4_test_case test_case = {
                .name = {.bytes = NULL, .byte_count = 0}};
            status = sxml_qt4_catalog_read_test_case(
                catalog,
                &(sxml_qt4_test_case_request){
                    .test_case_index = test_case_index, .problem = NULL},
                &test_case);
            if (status == SXML_STATUS_MALFORMED_INPUT) {
                status = SXML_STATUS_OK;
            }
        }
    }
    sxml_qt4_catalog_destroy(catalog);
    return status;
}

static void
test_catalog_survives_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_create(
                sxml_default_allocator(),
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number},
                &counting_allocator) == SXML_STATUS_OK);
        const sxml_status status = walk_the_suite(
            sxml_counting_allocator_as_allocator(counting_allocator));
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        if (counts.live_allocation_count != 0 ||
            status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
        failing_allocation_number += 1;
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

static const sxml_test_case driver_test_cases[] = {
    {"suite_paths_resolve_against_the_containing_file",
     test_suite_paths_resolve_against_the_containing_file},
    {"spec_tokens_match_language_and_version",
     test_spec_tokens_match_language_and_version},
    {"dependency_table_lists_what_sxml_supports",
     test_dependency_table_lists_what_sxml_supports},
    {"applicability_follows_the_rules_in_order",
     test_applicability_follows_the_rules_in_order},
    {"scope_follows_the_non_goals_of_d95",
     test_scope_follows_the_non_goals_of_d95},
    {"resource_table_serves_resources_and_collections",
     test_resource_table_serves_resources_and_collections},
    {"resource_table_serves_modules", test_resource_table_serves_modules},
    {"test_lists_are_read_like_the_check_script",
     test_test_lists_are_read_like_the_check_script},
    {"catalog_resolves_test_cases_and_environments",
     test_catalog_resolves_test_cases_and_environments},
    {"catalog_reports_broken_and_missing_files",
     test_catalog_reports_broken_and_missing_files},
    {"catalog_lifecycle_on_caller_memory",
     test_catalog_lifecycle_on_caller_memory},
    {"catalog_survives_every_allocation_failure",
     test_catalog_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "qt4_driver",
            .cases = driver_test_cases,
            .case_count = SXML_TEST_COUNT(driver_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
