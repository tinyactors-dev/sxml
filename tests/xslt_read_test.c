/*
 * Unit tests for src/xslt_read.c and src/xslt_static.c: stylesheet
 * modules read into the stylesheet AST (XSLT 4.0, 3.3 to 3.11; D102,
 * D139).
 *
 * Golden file: tests/xslt_ast_test.c (an AST's text form compared with
 * the expected text).
 */
#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xslt_read_internal.h"

#include <stdio.h>
#include <string.h>

/* A processor with a table of modules, and what reading gave. */
typedef struct read_fixture {
    sxml_processor *processor;
    sxml_xslt_ast *ast;
    sxml_error_record record;
    sxml_status status;
} read_fixture;

typedef struct read_case {
    const char *text;
    /* Further modules, served by the table resolver. */
    const sxml_resource_entry *modules;
    size_t module_count;
    const sxml_variable_binding *static_parameters;
    size_t static_parameter_count;
} read_case;

static bool open_processor(read_fixture *fixture, const read_case *test_case) {
    memset(fixture, 0, sizeof(*fixture));
    const sxml_allocator allocator = sxml_default_allocator();
    static sxml_resource_entry_view entries;
    entries = (sxml_resource_entry_view){.resource_entries = test_case->modules,
                                         .resource_entry_count = test_case->module_count};
    return sxml_processor_create(
               allocator,
               &(sxml_processor_configuration){.allocator = allocator,
                                               .resource_resolver =
                                                   sxml_table_resource_resolver(&entries)},
               &fixture->processor) == SXML_STATUS_OK;
}

static void read_stylesheet(read_fixture *fixture, const read_case *test_case) {
    fixture->status = sxml_xslt_read_stylesheet(
        &(sxml_xslt_read_request){
            .processor = fixture->processor,
            .principal_module_text = sxml_byte_view_from_c_string(test_case->text),
            .principal_module_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///main.xsl"),
            .static_parameters = {.variable_bindings = test_case->static_parameters,
                                  .variable_binding_count = test_case->static_parameter_count},
            .error_record = &fixture->record},
        &fixture->ast);
}

static void close_fixture(read_fixture *fixture) {
    sxml_xslt_ast_destroy(fixture->ast);
    sxml_processor_destroy(fixture->processor);
}

static bool code_is(const read_fixture *fixture, const char *code) {
    return sxml_byte_view_equals(fixture->record.code.local_name,
                                 sxml_byte_view_from_c_string(code));
}

/* Reads text and compares the error code (NULL: success). */
static bool reads_with(const read_case *test_case, const char *code) {
    read_fixture fixture;
    if (!open_processor(&fixture, test_case)) {
        return false;
    }
    read_stylesheet(&fixture, test_case);
    bool matches = code == NULL ? fixture.status == SXML_STATUS_OK
                                : fixture.status != SXML_STATUS_OK && code_is(&fixture, code);
    if (!matches) {
        printf("status %d code %.*s: %.*s\n", (int)fixture.status,
               (int)fixture.record.code.local_name.byte_count,
               (const char *)fixture.record.code.local_name.bytes,
               (int)fixture.record.diagnostic.description.byte_count,
               (const char *)fixture.record.diagnostic.description.bytes);
    }
    close_fixture(&fixture);
    return matches;
}

static bool text_form_is(const read_case *test_case, const char *expected) {
    read_fixture fixture;
    if (!open_processor(&fixture, test_case)) {
        return false;
    }
    read_stylesheet(&fixture, test_case);
    static char output[16384];
    size_t byte_count = 0;
    bool matches = fixture.status == SXML_STATUS_OK &&
                   sxml_xslt_ast_write_text(fixture.ast,
                                            (sxml_memory_view){.bytes = (uint8_t *)output,
                                                               .byte_count = sizeof(output)},
                                            &byte_count) == SXML_STATUS_OK &&
                   byte_count == strlen(expected) && memcmp(output, expected, byte_count) == 0;
    if (!matches) {
        printf("status %d %.*s\ngot:\n%.*s\n", (int)fixture.status,
               (int)fixture.record.code.local_name.byte_count,
               (const char *)fixture.record.code.local_name.bytes, (int)byte_count, output);
    }
    close_fixture(&fixture);
    return matches;
}

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* The simplified stylesheet of the header's example 1, shortened. */
static void test_simplified_stylesheet(sxml_test_state *state) {
    const read_case test_case = {
        .text = "<table xsl:version='2.0' " XSL_NAMESPACE ">\n"
                "<!-- comment -->\n"
                "  <tr><th>Position</th></tr>\n"
                "  <xsl:for-each-group select='cities/city' group-by='@country'>\n"
                "    <xsl:if test='position() != last()'>, </xsl:if>\n"
                "  </xsl:for-each-group>\n"
                "</table>"};
    SXML_TEST_EXPECT(
        state,
        text_form_is(
            &test_case,
            "module 1 principal simplified precedence 1 root 1\n"
            "  uri \"file:///main.xsl\"\n"
            "context 1 parent 0 version 2.0 expand-text no\n"
            "  namespaces (xsl = http://www.w3.org/1999/XSL/Transform)\n"
            "  xpath-default-namespace none default-collation codepoint\n"
            "  default-mode #unnamed default-validation strip\n"
            "  excluded (http://www.w3.org/1999/XSL/Transform) extension ()\n"
            "  base-uri \"file:///main.xsl\"\n"
            "declarations (node 2)\n"
            "node 1 STYLESHEET module 1 synthesized context 1 at 1:1\n"
            "  node 2 TEMPLATE synthesized declaration 1 at 1:1\n"
            "    attribute MATCH synthesized pattern 1\n"
            "    node 3 LITERAL_RESULT_ELEMENT table at 1:1\n"
            "      attribute VERSION xslt-namespace text \"2.0\" at 1:8\n"
            "      node 4 LITERAL_RESULT_ELEMENT tr at 3:3\n"
            "        node 5 LITERAL_RESULT_ELEMENT th at 3:7\n"
            "          node 6 TEXT_NODE text \"Position\" at 3:11\n"
            "      node 7 FOR_EACH_GROUP at 4:3\n"
            "        attribute SELECT expression 2 \"cities/city\" at 4:23\n"
            "        attribute GROUP_BY expression 3 \"@country\" at 4:44\n"
            "        node 8 IF at 5:5\n"
            "          attribute TEST expression 4 \"position() != last()\" at 5:13\n"
            "          node 9 TEXT_NODE text \", \" at 5:41\n"));
}

static void test_stylesheet_with_text_value_templates(sxml_test_state *state) {
    const read_case test_case = {
        .text = "<xsl:stylesheet version='4.0' expand-text='yes' " XSL_NAMESPACE
                " xmlns:f='urn:f' xmlns:xs='http://www.w3.org/2001/XMLSchema'>\n"
                "  <xsl:template match='item' mode='list' priority='2'>\n"
                "    <li class='{@code}'>{f:label(.)}</li>\n"
                "  </xsl:template>\n"
                "  <xsl:function name='f:label' as='xs:string'>\n"
                "    <xsl:param name='item'/>\n"
                "    <xsl:sequence select='string($item)'/>\n"
                "  </xsl:function>\n"
                "</xsl:stylesheet>"};
    SXML_TEST_EXPECT(
        state,
        text_form_is(
            &test_case,
            "module 1 principal precedence 1 root 1\n"
            "  uri \"file:///main.xsl\"\n"
            "context 1 parent 0 version 4.0 expand-text yes\n"
            "  namespaces (xsl = http://www.w3.org/1999/XSL/Transform, f = urn:f, "
            "xs = http://www.w3.org/2001/XMLSchema)\n"
            "  xpath-default-namespace none default-collation codepoint\n"
            "  default-mode #unnamed default-validation strip\n"
            "  excluded (http://www.w3.org/1999/XSL/Transform) extension ()\n"
            "  base-uri \"file:///main.xsl\"\n"
            "declarations (node 2, node 5)\n"
            "node 1 STYLESHEET module 1 context 1 at 1:1\n"
            "  attribute VERSION text \"4.0\" at 1:17\n"
            "  attribute EXPAND_TEXT text \"yes\" at 1:31\n"
            "  node 2 TEMPLATE declaration 1 at 2:3\n"
            "    attribute MATCH pattern 1 \"item\" at 2:17\n"
            "    attribute MODE names (list) at 2:30\n"
            "    attribute PRIORITY number 2 at 2:42\n"
            "    node 3 LITERAL_RESULT_ELEMENT li at 3:5\n"
            "      attribute LITERAL_RESULT class value-template 2 \"{@code}\" at 3:9\n"
            "      node 4 TEXT_NODE value-template 3 \"{f:label(.)}\" at 3:25\n"
            "  node 5 FUNCTION declaration 2 at 5:3\n"
            "    attribute NAME name Q{urn:f}label at 5:17\n"
            "    attribute AS sequence-type 4 \"xs:string\" at 5:32\n"
            "    node 6 PARAM at 6:5\n"
            "      attribute NAME name item at 6:16\n"
            "    node 7 SEQUENCE at 7:5\n"
            "      attribute SELECT expression 5 \"string($item)\" at 7:19\n"));
}

/* use-when, static parameters and shadow attributes (3.11.3, 3.11.4, 9.6). */
static void test_static_expressions(sxml_test_state *state) {
    read_fixture fixture;
    const read_case test_case = {
        .text = "<xsl:stylesheet version='3.0' " XSL_NAMESPACE
                " xmlns:fn='http://www.w3.org/2005/xpath-functions'>\n"
                "  <xsl:param name='mode' static='yes' select='\"b\"'/>\n"
                "  <xsl:variable name='twice' static='yes' select='$mode || $mode'/>\n"
                "  <xsl:template name='a' use-when='$mode = \"a\"'/>\n"
                "  <xsl:template _name='{$twice}' use-when=\"system-property('xsl:version') = "
                "'4.0' and function-available('fn:string', 1) and "
                "element-available('xsl:iterate') and not(element-available('xsl:nothing'))\"/>\n"
                "</xsl:stylesheet>"};
    SXML_TEST_REQUIRE(state, open_processor(&fixture, &test_case));
    read_stylesheet(&fixture, &test_case);
    SXML_TEST_REQUIRE(state, fixture.status == SXML_STATUS_OK);
    /* param, variable, the second template: the first is excluded. */
    SXML_TEST_EXPECT(state, fixture.ast->declaration_count == 4);
    const sxml_xslt_ast_node_index template_node = sxml_xslt_ast_get_declaration(fixture.ast, 3);
    const sxml_xslt_ast_attribute *name = sxml_xslt_ast_get_attribute(
        fixture.ast, sxml_xslt_ast_find_attribute(
                         fixture.ast, (sxml_xslt_ast_attribute_query){
                                          .node = template_node,
                                          .kind = SXML_XSLT_AST_ATTRIBUTE_NAME}));
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, (name->flags & SXML_XSLT_AST_ATTRIBUTE_FLAG_SHADOW) != 0);
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                sxml_processor_get_name_pool(fixture.processor),
                                name->value.payload.name, &parts) == SXML_STATUS_OK &&
                                sxml_byte_view_equals(parts.local_name,
                                                      SXML_BYTE_VIEW_FROM_LITERAL("bb")));
    close_fixture(&fixture);

    /* A supplied static parameter wins, and is coerced to its type. */
    read_fixture supplied;
    SXML_TEST_REQUIRE(state, open_processor(&supplied, &test_case));
    sxml_name_identifier mode = {.index = 0};
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_name(
                                 sxml_processor_get_name_pool(supplied.processor),
                                 &(sxml_name_request){.namespace_uri = {.index = 0},
                                                      .local_name =
                                                          SXML_BYTE_VIEW_FROM_LITERAL("mode")},
                                 &mode) == SXML_STATUS_OK);
    sxml_sequence value = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_function_library_make_string(
                                 sxml_processor_get_item_heap(supplied.processor),
                                 SXML_BYTE_VIEW_FROM_LITERAL("a"), &value) == SXML_STATUS_OK);
    const sxml_variable_binding binding = {.variable_name = mode, .value = value};
    const read_case with_parameter = {.text = test_case.text,
                                      .static_parameters = &binding,
                                      .static_parameter_count = 1};
    read_stylesheet(&supplied, &with_parameter);
    SXML_TEST_EXPECT(state, supplied.status == SXML_STATUS_OK &&
                                supplied.ast->declaration_count == 5);
    sxml_sequence_release(value);
    close_fixture(&supplied);

    const read_case required = {
        .text = "<xsl:stylesheet version='3.0' " XSL_NAMESPACE ">"
                "<xsl:param name='p' static='yes' required='yes'/></xsl:stylesheet>"};
    SXML_TEST_EXPECT(state, reads_with(&required, "XTDE0050"));
    const read_case mistyped = {
        .text = "<xsl:stylesheet version='3.0' " XSL_NAMESPACE
                " xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                "<xsl:variable name='v' static='yes' select='\"x\"' as='xs:integer'/>"
                "</xsl:stylesheet>"};
    SXML_TEST_EXPECT(state, reads_with(&mistyped, "XTTE0570"));
}

/* Includes and imports through the resolver (3.10). */
static void test_includes_and_imports(sxml_test_state *state) {
    static const sxml_resource_entry modules[] = {
        {.uri = SXML_BYTE_VIEW_INITIALIZER("file:///base.xsl"),
         .content = SXML_BYTE_VIEW_INITIALIZER(
             "<xsl:stylesheet version='3.0' " XSL_NAMESPACE ">"
             "<xsl:include href='main.xsl'/><xsl:template name='b'/></xsl:stylesheet>")},
        {.uri = SXML_BYTE_VIEW_INITIALIZER("file:///loop.xsl"),
         .content = SXML_BYTE_VIEW_INITIALIZER(
             "<xsl:stylesheet version='3.0' " XSL_NAMESPACE ">"
             "<xsl:import href='main.xsl'/></xsl:stylesheet>")},
    };
    read_fixture fixture;
    const read_case including = {
        .text = "<xsl:stylesheet version='3.0' " XSL_NAMESPACE ">"
                "<xsl:include href='base.xsl'/><xsl:include href='base.xsl'/>"
                "<xsl:template name='m'/></xsl:stylesheet>",
        .modules = modules,
        .module_count = 2};
    SXML_TEST_REQUIRE(state, open_processor(&fixture, &including));
    read_stylesheet(&fixture, &including);
    SXML_TEST_REQUIRE(state, fixture.status == SXML_STATUS_OK);
    /* base.xsl once; its include of main.xsl is an include cycle, and the
       second include a duplicate: both ignored (4.0, 3.10.2). */
    SXML_TEST_EXPECT(state, fixture.ast->module_count == 3);
    SXML_TEST_EXPECT(state, fixture.ast->declaration_count == 6);
    close_fixture(&fixture);
    const read_case importing = {.text = "<xsl:stylesheet version='3.0' " XSL_NAMESPACE ">"
                                         "<xsl:import href='loop.xsl'/></xsl:stylesheet>",
                                 .modules = modules,
                                 .module_count = 2};
    SXML_TEST_EXPECT(state, reads_with(&importing, "XTSE0210"));
    const read_case missing = {.text = "<xsl:stylesheet version='3.0' " XSL_NAMESPACE ">"
                                       "<xsl:import href='none.xsl'/></xsl:stylesheet>",
                               .modules = modules,
                               .module_count = 2};
    SXML_TEST_EXPECT(state, reads_with(&missing, "XTSE0165"));
}

/* Static errors of the element syntax. */
static void test_static_errors(sxml_test_state *state) {
    typedef struct error_case {
        const char *text;
        const char *code;
    } error_case;
    static const error_case cases[] = {
        {"<xsl:stylesheet " XSL_NAMESPACE "/>", "XTSE0010"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/' bad='1'/>"
         "</xsl:stylesheet>",
         "XTSE0090"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:if test='1'/></xsl:stylesheet>",
         "XTSE0010"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
         "<xsl:value-of/><xsl:param name='p'/></xsl:template></xsl:stylesheet>",
         "XTSE0010"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
         "<xsl:choose/></xsl:template></xsl:stylesheet>",
         "XTSE0010"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
         "<xsl:copy-of select='.'>x</xsl:copy-of></xsl:template></xsl:stylesheet>",
         "XTSE0260"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE ">text</xsl:stylesheet>", "XTSE0120"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><top/></xsl:stylesheet>", "XTSE0130"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='a['/>"
         "</xsl:stylesheet>",
         "XTSE0340"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
         "<xsl:value-of select='1 +'/></xsl:template></xsl:stylesheet>",
         "XPST0003"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
         "<a b='{'/></xsl:template></xsl:stylesheet>",
         "XTSE0350"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template name='p:t'/>"
         "</xsl:stylesheet>",
         "XTSE0280"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/' "
         "priority='high'/></xsl:stylesheet>",
         "XTSE0530"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template mode='m'/>"
         "</xsl:stylesheet>",
         "XTSE0500"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:mode name='m'><xsl:template/>"
         "</xsl:mode></xsl:stylesheet>",
         "XTSE4010"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
         "<xsl:include href='x.xsl'/></xsl:template></xsl:stylesheet>",
         "XTSE0170"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:param name='p' required='yes' "
         "select='1'/></xsl:stylesheet>",
         "XTSE0010"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
         "<xsl:copy-of select='.' copy-namespaces='maybe'/></xsl:template></xsl:stylesheet>",
         "XTSE0020"},
        {"<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/' "
         "use-when='1 div 0'/></xsl:stylesheet>",
         "FOAR0001"},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index += 1) {
        const read_case test_case = {.text = cases[index].text};
        const bool matches = reads_with(&test_case, cases[index].code);
        if (!matches) {
            printf("case %zu (%s)\n", index, cases[index].code);
        }
        SXML_TEST_EXPECT(state, matches);
    }
}

/* Forwards-compatible mode (3.9): unknown elements and attributes. */
static void test_forwards_compatible(sxml_test_state *state) {
    const read_case test_case = {
        .text = "<xsl:stylesheet version='5.0' " XSL_NAMESPACE ">"
                "<xsl:future-declaration/>"
                "<xsl:template match='/' future='1'><xsl:future><xsl:fallback>x</xsl:fallback>"
                "</xsl:future></xsl:template></xsl:stylesheet>"};
    SXML_TEST_EXPECT(state, reads_with(&test_case, NULL));
    const read_case outside = {
        .text = "<xsl:stylesheet version='3.0' " XSL_NAMESPACE "><xsl:template match='/'>"
                "<xsl:future/></xsl:template></xsl:stylesheet>"};
    SXML_TEST_EXPECT(state, reads_with(&outside, "XTSE0010"));
}

static const sxml_test_case xslt_read_test_cases[] = {
    {.name = "simplified_stylesheet", .function = test_simplified_stylesheet},
    {.name = "stylesheet_with_text_value_templates",
     .function = test_stylesheet_with_text_value_templates},
    {.name = "static_expressions", .function = test_static_expressions},
    {.name = "includes_and_imports", .function = test_includes_and_imports},
    {.name = "static_errors", .function = test_static_errors},
    {.name = "forwards_compatible", .function = test_forwards_compatible},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_read",
            .cases = xslt_read_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_read_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
