/*
 * Tests for the HTML parser (src/html_internal.h; packet 7.10a): the
 * tokenizer (HTML Living Standard, 13.2.5), tree construction (13.2.6)
 * on the working tree, the input stream (13.2.3), and fn:parse-html and
 * fn:html-doc through a processor with and without the feature (F&O 4.0,
 * 17.3), with an allocation-failure loop (D23). Every case checks for
 * leaks.
 *
 * Golden file: tests/optional_feature_test.c.
 */
#include "html_internal.h"
#include "optional_feature_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static sxml_counting_allocator *make_counting_allocator(uint64_t failing_number) {
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                     .failing_allocation_number = failing_number},
            &counting_allocator) != SXML_STATUS_OK) {
        return NULL;
    }
    return counting_allocator;
}

static uint64_t live_allocations(sxml_counting_allocator *counting_allocator) {
    return sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count;
}

/* ------------------------------------------------------------ Tokenizer */

/* Writes the tokens of input as text: one line per token. */
static sxml_status describe_tokens(sxml_byte_view input, sxml_xml_buffer *description) {
    sxml_html_tokenizer tokenizer;
    sxml_html_tokenizer_initialize(
        &tokenizer, &(sxml_html_tokenizer_configuration){.allocator = description->allocator,
                                                         .input = input});
    sxml_status status = SXML_STATUS_OK;
    bool is_at_end = false;
    while (status == SXML_STATUS_OK && !is_at_end) {
        sxml_html_token token;
        status = sxml_html_tokenizer_next(&tokenizer, &token);
        if (status != SXML_STATUS_OK) {
            break;
        }
        static const char *const kind_names[] = {"chars", "start", "end", "comment",
                                                 "doctype", "pi", "eof"};
        const char *kind_name = kind_names[token.token_kind];
        status = sxml_xml_buffer_append(
            description, (sxml_byte_view){.bytes = (const uint8_t *)kind_name,
                                          .byte_count = strlen(kind_name)});
        const sxml_byte_view parts[] = {token.name, token.data, token.public_identifier,
                                        token.system_identifier};
        for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < 4;
             part_index += 1) {
            if (parts[part_index].byte_count > 0) {
                status = sxml_xml_buffer_append_byte(description, ' ');
                if (status == SXML_STATUS_OK) {
                    status = sxml_xml_buffer_append(description, parts[part_index]);
                }
            }
        }
        for (size_t attribute_index = 0;
             status == SXML_STATUS_OK && attribute_index < token.attributes.attribute_count;
             attribute_index += 1) {
            const sxml_html_token_attribute *attribute =
                &token.attributes.attributes[attribute_index];
            status = sxml_xml_buffer_append_byte(description, ' ');
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append(description, attribute->attribute_name);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append_byte(description, '=');
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append(description, attribute->attribute_value);
            }
        }
        if (status == SXML_STATUS_OK && token.is_self_closing) {
            status = sxml_xml_buffer_append(description, TEXT(" /"));
        }
        if (status == SXML_STATUS_OK && token.forces_quirks) {
            status = sxml_xml_buffer_append(description, TEXT(" quirks"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(description, '\n');
        }
        is_at_end = token.token_kind == SXML_HTML_TOKEN_KIND_END_OF_FILE;
    }
    sxml_html_tokenizer_finalize(&tokenizer);
    return status;
}

/* An input and the tokens it gives. */
typedef struct token_case {
    sxml_byte_view input;
    sxml_byte_view tokens;
} token_case;

#define TOKEN_CASE(input_text, tokens_text)                                   \
    {.input = SXML_BYTE_VIEW_INITIALIZER(input_text),                         \
     .tokens = SXML_BYTE_VIEW_INITIALIZER(tokens_text)}

static const token_case token_cases[] = {
    TOKEN_CASE("<p class=a id='b' title=\"c d\">x</p>",
               "start p class=a id=b title=c d\nchars x\nend p\neof\n"),
    /* 13.2.5.33: a duplicate attribute is dropped. */
    TOKEN_CASE("<A HREF=1 href=2>", "start a href=1\neof\n"),
    TOKEN_CASE("<br/>", "start br /\neof\n"),
    /* 13.2.5.78: the longest name, with or without its semicolon. */
    TOKEN_CASE("&amp;&lt&notin;&notit;&ampx", "chars &<\xE2\x88\x89\xC2\xACit;&x\neof\n"),
    /* "For historical reasons" no reference before = in attributes. */
    TOKEN_CASE("<a href='?a=1&copy=2&copy;'>", "start a href=?a=1&copy=2\xC2\xA9\neof\n"),
    /* 13.2.5.84: numeric references, C1 replacements. */
    TOKEN_CASE("&#65;&#x42;&#128;&#0;&#x110000;", "chars AB\xE2\x82\xAC\xEF\xBF\xBD\xEF\xBF\xBD\neof\n"),
    TOKEN_CASE("<!-- a -- b --><!-->", "comment  a -- b \ncomment\neof\n"),
    TOKEN_CASE("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" 'x'>",
               "doctype html -//W3C//DTD HTML 4.01//EN x\neof\n"),
    TOKEN_CASE("<!DOCTYPE>", "doctype quirks\neof\n"),
    /* 13.2.5.72: processing instructions; "xml" targets are comments. */
    TOKEN_CASE("<?target some data?><?xml version='1.0'?>",
               "pi target some data\ncomment ?xml version='1.0'?\neof\n"),
    TOKEN_CASE("</>x</ y>", "chars x\ncomment  y\neof\n"),
    TOKEN_CASE("<a b", "eof\n"),
    TOKEN_CASE("a<", "chars a<\neof\n"),
};

static void test_tokenizer_follows_the_states(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(token_cases); case_index += 1) {
        sxml_xml_buffer description = sxml_xml_buffer_empty(sxml_default_allocator());
        SXML_TEST_EXPECT(state, describe_tokens(token_cases[case_index].input, &description) ==
                                    SXML_STATUS_OK);
        const sxml_byte_view got = {.bytes = description.bytes,
                                    .byte_count = description.byte_count};
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(got, token_cases[case_index].tokens));
        sxml_xml_buffer_release(&description);
    }
}

/* Tree construction switches the tokenizer to its text states. */
static void test_tokenizer_reads_script_data(sxml_test_state *state) {
    sxml_html_tokenizer tokenizer;
    sxml_html_tokenizer_initialize(
        &tokenizer,
        &(sxml_html_tokenizer_configuration){
            .allocator = sxml_default_allocator(),
            .input = TEXT("<script>if (a<b) x='</p>'; <!--<script></script>--></script>")});
    sxml_html_token token;
    SXML_TEST_REQUIRE(state, sxml_html_tokenizer_next(&tokenizer, &token) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, token.token_kind == SXML_HTML_TOKEN_KIND_START_TAG);
    sxml_html_tokenizer_switch_state(&tokenizer, SXML_HTML_CONTENT_STATE_SCRIPT_DATA);
    SXML_TEST_REQUIRE(state, sxml_html_tokenizer_next(&tokenizer, &token) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, token.token_kind == SXML_HTML_TOKEN_KIND_CHARACTERS);
    /* The double escaped script data keeps the inner </script>. */
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                token.data, TEXT("if (a<b) x='</p>'; <!--<script></script>-->")));
    SXML_TEST_REQUIRE(state, sxml_html_tokenizer_next(&tokenizer, &token) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, token.token_kind == SXML_HTML_TOKEN_KIND_END_TAG);
    SXML_TEST_EXPECT(state, !tokenizer.has_parse_error);
    sxml_html_tokenizer_finalize(&tokenizer);
}

/* -------------------------------------------------------- Tree building */

static sxml_status append_text(sxml_xml_buffer *buffer, const char *text) {
    return sxml_xml_buffer_append(
        buffer, (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = strlen(text)});
}

/* Writes a node of the working tree, its start or (is_end) its end. */
typedef struct node_description_request {
    const sxml_html_working_tree *tree;
    uint32_t node_number;
    bool is_end;
} node_description_request;

static sxml_status describe_node(const node_description_request *request,
                                 sxml_xml_buffer *description) {
    const sxml_html_working_tree *tree = request->tree;
    const sxml_html_node *node = sxml_html_working_tree_node(tree, request->node_number);
    static const char *const namespace_prefixes[] = {"", "math:", "svg:"};
    static const char *const attribute_prefixes[] = {"", "xlink:", "xml:", "xmlns:"};
    sxml_status status = SXML_STATUS_OK;
    switch (node->node_kind) {
    case SXML_HTML_NODE_KIND_DOCUMENT:
        return SXML_STATUS_OK;
    case SXML_HTML_NODE_KIND_ELEMENT:
        status = append_text(description, request->is_end ? "</" : "<");
        if (status == SXML_STATUS_OK) {
            status = append_text(description, namespace_prefixes[node->element_namespace]);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(description, sxml_xml_buffer_view(&tree->text, node->name));
        }
        for (uint32_t attribute_index = 0;
             !request->is_end && status == SXML_STATUS_OK && attribute_index < node->attribute_count;
             attribute_index += 1) {
            const sxml_html_attribute *attribute =
                sxml_xml_array_at(&tree->attributes, (size_t)node->first_attribute + attribute_index);
            status = append_text(description, " ");
            if (status == SXML_STATUS_OK) {
                status = append_text(description, attribute_prefixes[attribute->attribute_namespace]);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append(
                    description, sxml_xml_buffer_view(&tree->text, attribute->attribute_name));
            }
            if (status == SXML_STATUS_OK) {
                status = append_text(description, "=");
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append(
                    description, sxml_xml_buffer_view(&tree->text, attribute->attribute_value));
            }
        }
        return status == SXML_STATUS_OK ? append_text(description, ">") : status;
    case SXML_HTML_NODE_KIND_TEXT:
        for (uint32_t segment_number = node->first_segment;
             status == SXML_STATUS_OK && segment_number != SXML_HTML_NO_NODE;) {
            const sxml_html_text_segment *segment = sxml_xml_array_at(&tree->segments, segment_number);
            status = sxml_xml_buffer_append(description,
                                            sxml_xml_buffer_view(&tree->text, segment->text_range));
            segment_number = segment->next_segment;
        }
        return status;
    case SXML_HTML_NODE_KIND_COMMENT:
        status = append_text(description, "<!--");
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(description, sxml_xml_buffer_view(&tree->text, node->data));
        }
        return status == SXML_STATUS_OK ? append_text(description, "-->") : status;
    case SXML_HTML_NODE_KIND_PROCESSING_INSTRUCTION:
        status = append_text(description, "<?");
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(description, sxml_xml_buffer_view(&tree->text, node->name));
        }
        return status == SXML_STATUS_OK ? append_text(description, "?>") : status;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Writes the working tree as markup, walking it by its links. */
static sxml_status describe_tree(const sxml_html_working_tree *tree, sxml_xml_buffer *description) {
    uint32_t node_number = sxml_html_working_tree_node(tree, 0)->first_child;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && node_number != SXML_HTML_NO_NODE) {
        status = describe_node(&(node_description_request){.tree = tree,
                                                           .node_number = node_number,
                                                           .is_end = false},
                               description);
        const sxml_html_node *node = sxml_html_working_tree_node(tree, node_number);
        if (node->first_child != SXML_HTML_NO_NODE) {
            node_number = node->first_child;
            continue;
        }
        if (status == SXML_STATUS_OK && node->node_kind == SXML_HTML_NODE_KIND_ELEMENT) {
            status = describe_node(&(node_description_request){.tree = tree,
                                                               .node_number = node_number,
                                                               .is_end = true},
                                   description);
        }
        while (status == SXML_STATUS_OK && node_number != SXML_HTML_NO_NODE) {
            const sxml_html_node *current = sxml_html_working_tree_node(tree, node_number);
            if (current->next_sibling != SXML_HTML_NO_NODE) {
                node_number = current->next_sibling;
                break;
            }
            node_number = current->parent;
            if (node_number == 0) {
                node_number = SXML_HTML_NO_NODE;
                break;
            }
            status = describe_node(&(node_description_request){.tree = tree,
                                                               .node_number = node_number,
                                                               .is_end = true},
                                   description);
        }
    }
    return status;
}

/* Parses input (a string) and writes its tree; *has_parse_error says
   whether a parse error was found. */
typedef struct tree_run {
    sxml_allocator allocator;
    sxml_byte_view input;
    bool has_parse_error;
} tree_run;

static sxml_status build_described_tree(tree_run *run, sxml_xml_buffer *description) {
    sxml_xml_buffer stream = sxml_xml_buffer_empty(run->allocator);
    sxml_html_input_outcome input_outcome;
    sxml_status status = sxml_html_prepare_input(
        &(sxml_html_input_request){.input = run->input,
                                   .is_string = true,
                                   .encoding_name = {.bytes = NULL, .byte_count = 0}},
        &stream, &input_outcome);
    sxml_html_working_tree tree;
    memset(&tree, 0, sizeof(tree));
    if (status == SXML_STATUS_OK) {
        status = sxml_html_working_tree_initialize(&tree, run->allocator);
    }
    sxml_html_parse_outcome outcome = {.has_parse_error = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_html_construct_tree(
            &(sxml_html_parse_request){
                .input = {.bytes = stream.bytes, .byte_count = stream.byte_count},
                .stops_at_parse_error = false},
            &tree, &outcome);
    }
    if (status == SXML_STATUS_OK) {
        status = describe_tree(&tree, description);
    }
    run->has_parse_error = outcome.has_parse_error || input_outcome.has_parse_error;
    sxml_html_working_tree_finalize(&tree);
    sxml_xml_buffer_release(&stream);
    return status;
}

/* An input, its tree and whether it has a parse error. */
typedef struct tree_case {
    sxml_byte_view input;
    sxml_byte_view tree;
    bool has_parse_error;
} tree_case;

#define TREE_CASE(input_text, tree_text, error)                               \
    {.input = SXML_BYTE_VIEW_INITIALIZER(input_text),                         \
     .tree = SXML_BYTE_VIEW_INITIALIZER(tree_text),                           \
     .has_parse_error = (error)}

static const tree_case tree_cases[] = {
    TREE_CASE("<!DOCTYPE html>", "<html><head></head><body></body></html>", false),
    /* 13.2.6.4.1: no DOCTYPE is a parse error. */
    TREE_CASE("<p>One<p>Two",
              "<html><head></head><body><p>One</p><p>Two</p></body></html>", true),
    TREE_CASE("<!DOCTYPE html><title>T&amp;</title><p>x",
              "<html><head><title>T&</title></head><body><p>x</p></body></html>", false),
    /* 13.2.10.1, 13.2.10.2: the adoption agency algorithm. */
    TREE_CASE("<!DOCTYPE html><b>1<p>2</b>3</p>",
              "<html><head></head><body><b>1</b><p><b>2</b>3</p></body></html>", true),
    TREE_CASE("<!DOCTYPE html><b><i></b></i>x",
              "<html><head></head><body><b><i></i></b>x</body></html>", true),
    /* 13.2.10.3: foster parenting moves text and elements before the table. */
    TREE_CASE("<!DOCTYPE html><table>a<tr>b<td>c</td>d</tr>e</table>",
              "<html><head></head><body>abde<table><tbody><tr><td>c</td></tr></tbody></table>"
              "</body></html>",
              true),
    /* 13.2.6.5: SVG names in their case, foreign attributes, breakout. */
    TREE_CASE("<!DOCTYPE html><svg viewbox='0 0 1 1' xlink:href=x><foreignobject><p>h</p>"
              "</foreignobject><clippath/></svg><math definitionurl=u><mi>x</mi></math>",
              "<html><head></head><body><svg:svg viewBox=0 0 1 1 xlink:href=x><svg:foreignObject>"
              "<p>h</p></svg:foreignObject><svg:clipPath></svg:clipPath></svg:svg>"
              "<math:math definitionURL=u><math:mi>x</math:mi></math:math></body></html>",
              false),
    TREE_CASE("<!DOCTYPE html><svg><p>x", "<html><head></head><body><svg:svg></svg:svg><p>x</p>"
                                          "</body></html>",
              true),
    /* A CDATA section is text in foreign content and a comment in HTML. */
    TREE_CASE("<!DOCTYPE html><svg><![CDATA[a<b]]></svg><![CDATA[c]]>",
              "<html><head></head><body><svg:svg>a<b</svg:svg><!--[CDATA[c]]--></body></html>",
              true),
    /* The template's contents are its children (F&O 4.0, 17.3.1). */
    TREE_CASE("<!DOCTYPE html><template><tr><td>x</template>",
              "<html><head><template><tr><td>x</td></tr></template></head><body></body></html>",
              false),
    TREE_CASE("<!DOCTYPE html><frameset><frame></frameset>",
              "<html><head></head><frameset><frame></frame></frameset></html>", false),
    /* Quirks mode: a table does not close an open p. */
    TREE_CASE("<p><table></table>", "<html><head></head><body><p><table></table></p></body></html>",
              true),
    TREE_CASE("<!DOCTYPE html><p><table></table>",
              "<html><head></head><body><p></p><table></table></body></html>", false),
    TREE_CASE("<!DOCTYPE html><pre>\nx</pre><textarea>\ny</textarea>",
              "<html><head></head><body><pre>x</pre><textarea>y</textarea></body></html>", false),
    TREE_CASE("<!DOCTYPE html><select><option>a<option>b</select>",
              "<html><head></head><body><select><option>a</option><option>b</option></select>"
              "</body></html>",
              false),
    TREE_CASE("<!DOCTYPE html><?pi data?><!--c-->",
              "<?pi?><!--c--><html><head></head><body></body></html>", false),
    TREE_CASE("<!DOCTYPE html><body a=1><body a=2 b=3>",
              "<html><head></head><body a=1 b=3></body></html>", true),
    TREE_CASE("x\r\ny\rz", "<html><head></head><body>x\ny\nz</body></html>", true),
};

static void test_tree_construction_follows_the_insertion_modes(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(tree_cases); case_index += 1) {
        sxml_xml_buffer description = sxml_xml_buffer_empty(sxml_default_allocator());
        tree_run run = {.allocator = sxml_default_allocator(),
                        .input = tree_cases[case_index].input,
                        .has_parse_error = false};
        SXML_TEST_EXPECT(state, build_described_tree(&run, &description) == SXML_STATUS_OK);
        const sxml_byte_view got = {.bytes = description.bytes,
                                    .byte_count = description.byte_count};
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(got, tree_cases[case_index].tree));
        SXML_TEST_EXPECT(state, run.has_parse_error == tree_cases[case_index].has_parse_error);
        sxml_xml_buffer_release(&description);
    }
}

static void test_tree_construction_survives_every_allocation_failure(sxml_test_state *state) {
    const sxml_byte_view input =
        TEXT("<!DOCTYPE html><table>a<tr><td><b>x<p>y</b>z<svg><desc>d</desc></svg>"
             "<template><td>t</template></table><!--c-->");
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    for (uint64_t failing_number = 1; failing_number < 10000 && status != SXML_STATUS_OK;
         failing_number += 1) {
        sxml_counting_allocator *counting_allocator = make_counting_allocator(failing_number);
        SXML_TEST_REQUIRE(state, counting_allocator != NULL);
        const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
        sxml_xml_buffer description = sxml_xml_buffer_empty(allocator);
        tree_run run = {.allocator = allocator, .input = input, .has_parse_error = false};
        status = build_described_tree(&run, &description);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        sxml_xml_buffer_release(&description);
        SXML_TEST_EXPECT(state, live_allocations(counting_allocator) == 0);
        sxml_counting_allocator_destroy(counting_allocator);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
}

/* --------------------------------------------------------- Input stream */

/* Octets, their encoding option, and the stream they give. */
typedef struct input_case {
    sxml_byte_view input;
    sxml_byte_view encoding_name;
    sxml_byte_view stream;
    sxml_html_input_failure failure;
} input_case;

#define INPUT_CASE(input_text, encoding_text, stream_text)                    \
    {.input = SXML_BYTE_VIEW_INITIALIZER(input_text),                         \
     .encoding_name = SXML_BYTE_VIEW_INITIALIZER(encoding_text),              \
     .stream = SXML_BYTE_VIEW_INITIALIZER(stream_text),                       \
     .failure = SXML_HTML_INPUT_FAILURE_NONE}

static const input_case input_cases[] = {
    INPUT_CASE("a\xE9z", "", "a\xC3\xA9z"),
    INPUT_CASE("\x80", "latin1", "\xE2\x82\xAC"),
    INPUT_CASE("\xCC\xEE", " Windows-1251 ", "\xD0\x9C\xD0\xBE"),
    INPUT_CASE("\xFF\xFE" "a\0\r\0\n\0", "", "a\n"),
    INPUT_CASE("\xFE\xFF\0b\xD8\x00", "", "b\xEF\xBF\xBD"),
    INPUT_CASE("\xEF\xBB\xBFx\xE2\x82", "windows-1252", "x\xEF\xBF\xBD"),
    /* The multi-byte decoders (Encoding Standard, 10 to 13; packet
       7.10b): two-byte and four-byte gb18030, the Big5 pointers with two
       code points and with a plane 2 code point, EUC-JP's katakana,
       Shift_JIS's EUDC, ISO-2022-JP's escapes, and errors that give the
       ASCII byte back. */
    INPUT_CASE("\xC4\xE3\x80\x81\x30\x81\x30\x94\x39\xFC\x36", "GBK",
               "\xE4\xBD\xA0\xE2\x82\xAC\xC2\x80\xF0\x9F\x98\x80"),
    INPUT_CASE("\x81\x30\x81 ", "gb18030", "\xEF\xBF\xBD" "0\xEF\xBF\xBD "),
    INPUT_CASE("\xA4\x40\x88\x62\x87\x45\x81" "a", "big5",
               "\xE4\xB8\x80\xC3\x8A\xCC\x84\xF0\xA7\x89\xA7\xEF\xBF\xBD" "a"),
    INPUT_CASE("\xA4\xA2\x8E\xB1", "euc-jp", "\xE3\x81\x82\xEF\xBD\xB1"),
    INPUT_CASE("\x82\xA0\xB1\xF0\x40", "shift_jis", "\xE3\x81\x82\xEF\xBD\xB1\xEE\x80\x80"),
    INPUT_CASE("a\x1B$B$\"\x1B(J\\\x1B(Bb", "iso-2022-jp", "a\xE3\x81\x82\xC2\xA5" "b"),
    INPUT_CASE("\x1B$B\x1B(B", "csiso2022jp", "\xEF\xBF\xBD"),
    INPUT_CASE("\xB0\xA1\xFF", "korean", "\xEA\xB0\x80\xEF\xBF\xBD"),
    /* No encoding given: the prescan finds a meta element (13.2.3.2). */
    INPUT_CASE("<meta charset=windows-1251>\xCC", "", "<meta charset=windows-1251>\xD0\x9C"),
    INPUT_CASE("<meta http-equiv=content-type content='text/html;charset=koi8-r'>\xED", "",
               "<meta http-equiv=content-type content='text/html;charset=koi8-r'>\xD0\x9C"),
    INPUT_CASE("<?xml version='1.0' encoding='iso-8859-7'?>\xE1", "",
               "<?xml version='1.0' encoding='iso-8859-7'?>\xCE\xB1"),
    {.input = SXML_BYTE_VIEW_INITIALIZER("x"),
     .encoding_name = SXML_BYTE_VIEW_INITIALIZER("no-such-encoding"),
     .stream = SXML_BYTE_VIEW_INITIALIZER(""),
     .failure = SXML_HTML_INPUT_FAILURE_UNKNOWN_ENCODING},
};

static void test_input_stream_decodes_octets(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(input_cases); case_index += 1) {
        const input_case *input = &input_cases[case_index];
        sxml_xml_buffer stream = sxml_xml_buffer_empty(sxml_default_allocator());
        sxml_html_input_outcome outcome;
        SXML_TEST_EXPECT(state, sxml_html_prepare_input(
                                    &(sxml_html_input_request){.input = input->input,
                                                               .is_string = false,
                                                               .encoding_name = input->encoding_name},
                                    &stream, &outcome) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, outcome.failure == input->failure);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals((sxml_byte_view){.bytes = stream.bytes,
                                                                        .byte_count =
                                                                            stream.byte_count},
                                                       input->stream));
        sxml_xml_buffer_release(&stream);
    }
}

/* ----------------------------------------------------- The XPath level */

static const sxml_resource_entry html_resource_entries[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/page.html"),
     .content = SXML_BYTE_VIEW_INITIALIZER(
         "<!DOCTYPE html><meta charset=windows-1251><p id=p1>\xCC\xEE</p>")},
};

static const sxml_resource_entry_view html_resources = {.resource_entries = html_resource_entries,
                                                        .resource_entry_count = 1};

/* A processor with or without the feature, and an output buffer. */
typedef struct html_fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    sxml_error_record error_record;
} html_fixture;

typedef struct html_fixture_options {
    bool has_feature;
    uint64_t failing_number;
} html_fixture_options;

static sxml_status html_fixture_create(html_fixture *fixture, html_fixture_options options) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->counting_allocator = make_counting_allocator(options.failing_number);
    if (fixture->counting_allocator == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(fixture->counting_allocator);
    const sxml_optional_feature feature = sxml_html_parser_feature();
    sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = sxml_table_resource_resolver(&html_resources),
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = options.has_feature ? 1U : 0U}},
        &fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when nothing leaked. */
static bool html_fixture_destroy(html_fixture *fixture) {
    bool has_no_leak = true;
    if (fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(fixture->processor);
    sxml_output_buffer_destroy(fixture->output_buffer);
    if (fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && live_allocations(fixture->counting_allocator) == 0;
        sxml_counting_allocator_destroy(fixture->counting_allocator);
    }
    return has_no_leak;
}

/* Evaluates expression_text and writes its value by the adaptive method
   into the fixture's output buffer. */
static sxml_status run_expression(html_fixture *fixture, sxml_byte_view expression_text) {
    memset(&fixture->error_record, 0, sizeof(fixture->error_record));
    sxml_output_buffer_clear(fixture->output_buffer);
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(fixture->counting_allocator);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        allocator,
        &(sxml_xpath_expression_configuration){.processor = fixture->processor,
                                               .expression_text = expression_text,
                                               .source_name = TEXT("test.xpath"),
                                               .static_context = {.static_base_uri = TEXT(
                                                                      "http://example.com/")},
                                               .error_record = &fixture->error_record},
        &expression);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){.error_record = &fixture->error_record}, &result);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(fixture->processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = true,
                                             .item_separator = TEXT(" ")},
                .error_record = &fixture->error_record},
            sxml_output_buffer_as_output_stream(fixture->output_buffer));
    }
    sxml_sequence_release(result);
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* An expression and its adaptive output, or the error code it raises. */
typedef struct expression_case {
    sxml_byte_view expression_text;
    sxml_byte_view output;
    sxml_byte_view error_code;
} expression_case;

#define EXPRESSION_VALUE(text, output_text)                                   \
    {.expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                     \
     .output = SXML_BYTE_VIEW_INITIALIZER(output_text),                       \
     .error_code = {.bytes = NULL, .byte_count = 0}}

#define EXPRESSION_ERROR(text, code)                                          \
    {.expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                     \
     .output = {.bytes = NULL, .byte_count = 0},                              \
     .error_code = SXML_BYTE_VIEW_INITIALIZER(code)}

static const expression_case expression_cases[] = {
    EXPRESSION_VALUE("parse-html('<p>Hello</p>')",
                     "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head/><body><p>Hello</p>"
                     "</body></html>"),
    EXPRESSION_VALUE("parse-html(())", ""),
    EXPRESSION_VALUE("namespace-uri(parse-html('<svg><circle/></svg>')//*:circle)",
                     "\"http://www.w3.org/2000/svg\""),
    EXPRESSION_VALUE("parse-html('<svg xlink:href=\"a\"/>')//@*!node-name()!string()",
                     "\"xlink:href\""),
    /* 17.3.1.1: namespace declarations are not attributes. */
    EXPRESSION_VALUE("count(parse-html('<p xmlns=\"x\" xmlns:a=\"y\" b=\"c\"></p>')//*:p/@*)",
                     "1"),
    /* 13.2.9: names XML does not allow, comments with "--". */
    EXPRESSION_VALUE("local-name(parse-html('<a:b>x</a:b>')//*:body/*)", "\"aU00003Ab\""),
    EXPRESSION_VALUE("string(parse-html('<!--a--b-->')/comment())", "\"a- -b\""),
    /* 17.3.1.5: an id that is an NCName is an ID. */
    EXPRESSION_VALUE("parse-html('<p id=x>1</p><p id=\"1 2\">2</p>')!(id('x')/string(), "
                     "count(id('1 2')))",
                     "\"1\" 0"),
    EXPRESSION_VALUE("string(parse-html(xs:hexBinary('3C703EE93C2F703E'))//*:p)", "\"\xC3\xA9\""),
    EXPRESSION_VALUE("string(parse-html(xs:hexBinary('3C703EE93C2F703E'), {'encoding': "
                     "'windows-1251'})//*:p)",
                     "\"\xD0\xB9\""),
    EXPRESSION_VALUE("string(html-doc('page.html', {'encoding': 'windows-1251'})//*:p)",
                     "\"\xD0\x9C\xD0\xBE\""),
    EXPRESSION_VALUE("document-uri(html-doc('page.html'))", "\"http://example.com/page.html\""),
    /* F&O 4.0, fn:parse-html: XHTML is recognized and parsed as XML: no
       head is implied, processing instructions and prefixes stay. */
    EXPRESSION_VALUE("parse-html('<?xml version=\"1.0\"?><!DOCTYPE html><html "
                     "xmlns=\"http://www.w3.org/1999/xhtml\"><body><?pi x?><svg:svg "
                     "xmlns:svg=\"http://www.w3.org/2000/svg\"/></body></html>')!(count(//*:head), "
                     "//processing-instruction()!name(), name(//*:svg))",
                     "0 \"pi\" \"svg:svg\""),
    /* Not well-formed: HTML after all. */
    EXPRESSION_VALUE("count(parse-html('<html xmlns=\"http://www.w3.org/1999/xhtml\"><p>a<br>"
                     "</html>')//*:head)",
                     "1"),
    /* 13.2.3.2: the prescan finds the encoding of a meta element. */
    EXPRESSION_VALUE("string(parse-html(xs:hexBinary("
                     "'3C6D65746120636861727365743D73686966745F6A69733E82A0'))//*:body)",
                     "\"\xE3\x81\x82\""),
    /* 13.2.3.4: a meta element after the prescan's 1024 bytes changes the
       encoding, and the document is parsed again. */
    EXPRESSION_VALUE("string(parse-html(xs:hexBinary(string-join((1 to 1100) ! '20') || "
                     "'3C6D65746120636861727365743D77696E646F77732D313235313ECC'), "
                     "{'fail-on-error': false()})//*:body)",
                     "\"\xD0\x9C\""),
    EXPRESSION_VALUE("parse-html('<!DOCTYPE html><p>x', {'fail-on-error': true()})//*:p/string()",
                     "\"x\""),
    EXPRESSION_ERROR("parse-html('<p>x', {'fail-on-error': true()})", "FODC0011"),
    EXPRESSION_ERROR("parse-html(xs:hexBinary('41'), {'encoding': 'nope'})", "FODC0011"),
    EXPRESSION_ERROR("parse-html('', {'include-template-content': true()})", "XPTY0004"),
    EXPRESSION_ERROR("parse-html('', {'encoding': true()})", "XPTY0004"),
    EXPRESSION_ERROR("html-doc('missing.html')", "FOUT1170"),
};

static void test_functions_parse_html(sxml_test_state *state) {
    html_fixture fixture;
    SXML_TEST_REQUIRE(state, html_fixture_create(&fixture, (html_fixture_options){
                                                               .has_feature = true,
                                                               .failing_number = 0}) ==
                                 SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(expression_cases);
         case_index += 1) {
        const expression_case *expression = &expression_cases[case_index];
        const sxml_status status = run_expression(&fixture, expression->expression_text);
        if (expression->error_code.byte_count == 0) {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                        sxml_output_buffer_get_bytes(fixture.output_buffer),
                                        expression->output));
        } else {
            SXML_TEST_EXPECT(state, status != SXML_STATUS_OK);
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(fixture.error_record.code.local_name,
                                                          expression->error_code));
        }
    }
    SXML_TEST_EXPECT(state, html_fixture_destroy(&fixture));
}

/* Without the feature the functions are absent (D191). */
static void test_functions_are_absent_without_the_feature(sxml_test_state *state) {
    html_fixture fixture;
    SXML_TEST_REQUIRE(state, html_fixture_create(&fixture, (html_fixture_options){
                                                               .has_feature = false,
                                                               .failing_number = 0}) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run_expression(&fixture, TEXT("parse-html('<p>')")) != SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(fixture.error_record.code.local_name, TEXT("XPST0017")));
    SXML_TEST_EXPECT(state, run_expression(&fixture, TEXT("empty(function-lookup(xs:QName('fn:html-doc'), 1))")) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_output_buffer_get_bytes(fixture.output_buffer),
                                                  TEXT("true()")));
    SXML_TEST_EXPECT(state, html_fixture_destroy(&fixture));
}

static void test_functions_survive_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    for (uint64_t failing_number = 1; failing_number < 100000 && status != SXML_STATUS_OK;
         failing_number += 1) {
        html_fixture fixture;
        status = html_fixture_create(&fixture, (html_fixture_options){
                                                   .has_feature = true,
                                                   .failing_number = failing_number});
        if (status == SXML_STATUS_OK) {
            status = run_expression(
                &fixture, TEXT("parse-html('<table><b>x<p>y</b><svg><a xlink:href=\"u\"/></svg>')"
                               "//*:p/string(), html-doc('page.html')//@id/string(), "
                               "parse-html('<html xmlns=\"http://www.w3.org/1999/xhtml\"><p>z</p>"
                               "</html>')//*:p/string(), "
                               "parse-html(xs:hexBinary(string-join((1 to 1030) ! '20') || "
                               "'3C6D65746120636861727365743D6B6F69382D723EED'))//*:body/string()"));
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                        sxml_output_buffer_get_bytes(fixture.output_buffer),
                                        TEXT("\"y\" \"p1\" \"z\" \"\xD0\x9C\"")));
        }
        SXML_TEST_EXPECT(state, html_fixture_destroy(&fixture));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
}

static const sxml_test_case html_test_cases[] = {
    {.name = "tokenizer_follows_the_states", .function = test_tokenizer_follows_the_states},
    {.name = "tokenizer_reads_script_data", .function = test_tokenizer_reads_script_data},
    {.name = "tree_construction_follows_the_insertion_modes",
     .function = test_tree_construction_follows_the_insertion_modes},
    {.name = "tree_construction_survives_every_allocation_failure",
     .function = test_tree_construction_survives_every_allocation_failure},
    {.name = "input_stream_decodes_octets", .function = test_input_stream_decodes_octets},
    {.name = "functions_parse_html", .function = test_functions_parse_html},
    {.name = "functions_are_absent_without_the_feature",
     .function = test_functions_are_absent_without_the_feature},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "html",
            .cases = html_test_cases,
            .case_count = SXML_TEST_COUNT(html_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
