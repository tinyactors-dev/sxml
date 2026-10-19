/*
 * The html5lib tests through the HTML parser (src/html_internal.h;
 * packets 7.10a and 7.10b, D201, D13): the tree construction tests of
 * web-platform-tests (html/syntax/parsing/resources, the .dat format of
 * its README) and the tokenizer tests of html5lib-tests (tokenizer/, the
 * JSON format of its README), vendored under third_party/html5lib-tests
 * at the commits that third_party/html5lib-tests.commit names.
 *
 * Golden file: tests/xml_conformance_test.c (a vendored suite read from
 * the source tree, found from this file's own path; where it cannot be
 * opened the test says so and passes).
 *
 * Tree construction: every test of the listed files runs as a document
 * (HTML Living Standard, 13.2.6) with scripting disabled. Left out, and
 * counted apart: "#document-fragment" tests (sxml has no fragment case,
 * D201) and "#script-on" tests (scripting is disabled, 13.2.4.5). The
 * working tree is written in the format of the README and compared with
 * "#document", without its DOCTYPE lines: the working tree keeps no
 * DocumentType node, which F&O 4.0, 17.3.1 maps to nothing.
 *
 * Tokenizer: every test in every initial state it names (13.2.5); a
 * test whose input or output holds a lone surrogate after "doubleEscaped"
 * unescaping is left out and counted apart: the input stream is UTF-8,
 * which cannot hold one. The CDATA section state is entered by "<![CDATA["
 * in front of the input with the adjusted current node foreign
 * (13.2.5.42), the "lastStartTag" through the tokenizer's last start tag.
 *
 * Inputs go through the input stream (13.2.3.5, newlines normalized) as
 * strings, with one 'a' in front that is cut off afterwards, so that a
 * leading U+FEFF stays: F&O 4.0's rule that strips it (fn:parse-html) is
 * not the tests'. sxml records whether there was a parse error, not which
 * (13.2.2), so that is what is compared: in the tokenizer tests it must
 * agree; in the tree construction tests sxml must find one wherever the
 * test lists one (fail-on-error depends on it), while some files
 * (processing-instructions.dat, html5test-com.dat, parts of webkit02.dat)
 * leave "#errors" empty for inputs with parse errors, such as a missing
 * DOCTYPE, so an error sxml finds there is only counted.
 *
 * The cases that fail are listed below with their reasons; any other
 * failure, and a listed case that passes, fails the test.
 */
#include "html_internal.h"
#include "sxml/memory.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

enum {
    SXML_TEST_PATH_BYTE_COUNT = 4096,
    SXML_TEST_NAME_BYTE_COUNT = 128,
};

#define SXML_TEST_NO_VALUE SIZE_MAX

/* ------------------------------------------------------------ Files */

/* Reads a whole file into contents (an empty buffer over an allocator).
   Returns false when it cannot be read. */
static bool read_file(const char *path, sxml_xml_buffer *contents) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    bool is_read = true;
    for (;;) {
        if (sxml_xml_buffer_reserve(contents, 1 << 16) != SXML_STATUS_OK) {
            is_read = false;
            break;
        }
        const size_t read_count = fread(contents->bytes + contents->byte_count, 1,
                                        contents->byte_capacity - contents->byte_count, file);
        contents->byte_count += read_count;
        if (read_count == 0) {
            break;
        }
    }
    is_read = is_read && ferror(file) == 0;
    (void)fclose(file);
    return is_read;
}

/* The suite's directory, from this file's path. */
static void suite_directory(char directory[SXML_TEST_PATH_BYTE_COUNT]) {
    const char *slash = strrchr(__FILE__, '/');
    const size_t byte_count = slash == NULL ? 0 : (size_t)(slash - __FILE__) + 1;
    (void)snprintf(directory, SXML_TEST_PATH_BYTE_COUNT, "%.*s../third_party/html5lib-tests/",
                   (int)byte_count, __FILE__);
}

/* ------------------------------------------------------------ Texts */

static sxml_byte_view view_of(const sxml_xml_buffer *buffer) {
    return (sxml_byte_view){.bytes = buffer->bytes, .byte_count = buffer->byte_count};
}

/* The offset of the first needle in haystack at or after start, or
   SXML_TEST_NO_VALUE. */
typedef struct text_search {
    sxml_byte_view haystack;
    size_t start;
} text_search;

static size_t find_text(text_search search, sxml_byte_view needle) {
    const sxml_byte_view haystack = search.haystack;
    for (size_t offset = search.start; offset + needle.byte_count <= haystack.byte_count;
         offset += 1) {
        if (memcmp(haystack.bytes + offset, needle.bytes, needle.byte_count) == 0) {
            return offset;
        }
    }
    return SXML_TEST_NO_VALUE;
}

static sxml_byte_view slice(sxml_byte_view text, sxml_byte_range range) {
    return (sxml_byte_view){.bytes = text.bytes + range.byte_offset,
                            .byte_count = range.byte_count};
}

static bool starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0;
}

/* The input stream of a test's input: newlines normalized (13.2.3.5),
   a leading U+FEFF kept. Appends to stream after prefix (bytes the
   tokenizer reads first). */
typedef struct stream_request {
    sxml_byte_view input;
    sxml_byte_view prefix;
    /* Set when preprocessing found a parse error (13.2.3.5). */
    bool *has_parse_error;
} stream_request;

static sxml_status make_stream(const stream_request *request, sxml_xml_buffer *stream) {
    sxml_xml_buffer marked = sxml_xml_buffer_empty(stream->allocator);
    sxml_status status = sxml_xml_buffer_append_byte(&marked, 'a');
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&marked, request->input);
    }
    sxml_xml_buffer prepared = sxml_xml_buffer_empty(stream->allocator);
    sxml_html_input_outcome outcome;
    if (status == SXML_STATUS_OK) {
        status = sxml_html_prepare_input(
            &(sxml_html_input_request){.input = view_of(&marked),
                                       .is_string = true,
                                       .encoding_name = {.bytes = NULL, .byte_count = 0}},
            &prepared, &outcome);
    }
    if (status == SXML_STATUS_OK) {
        *request->has_parse_error = outcome.has_parse_error;
        status = sxml_xml_buffer_append(stream, request->prefix);
    }
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xml_buffer_append(stream, (sxml_byte_view){.bytes = prepared.bytes + 1,
                                                            .byte_count = prepared.byte_count - 1});
    }
    sxml_xml_buffer_release(&prepared);
    sxml_xml_buffer_release(&marked);
    return status;
}

/* ------------------------------------------------------ Known failures */

/* A case that fails, and why. */
typedef struct known_failure {
    const char *case_name;
    const char *reason;
} known_failure;

static const known_failure known_failures[] = {
    /* An option's content is cloned into the select's selectedcontent by
       DOM steps of the option and select elements (HTML, 4.10.7, "maybe
       clone an option into selectedcontent"), which run when the option
       is inserted or popped; not by tree construction, which sxml
       implements (D201). */
    {.case_name = "webkit02.dat:45", .reason = "selectedcontent"},
    {.case_name = "webkit02.dat:46", .reason = "selectedcontent"},
    {.case_name = "webkit02.dat:47", .reason = "selectedcontent"},
    {.case_name = "webkit02.dat:48", .reason = "selectedcontent"},
};

/* What a run of one suite found. */
typedef struct suite_counts {
    size_t passed_count;
    size_t failed_count;
    size_t unexpected_count;
    size_t left_out_count;
    size_t error_agreement_count;
    size_t error_disagreement_count;
    /* The test expects a parse error and sxml finds none: what
       fail-on-error depends on (F&O 4.0, fn:parse-html). */
    size_t missed_error_count;
    /* Listed failures that passed. */
    size_t fixed_count;
} suite_counts;

static bool is_known_failure(const char *case_name) {
    for (size_t failure_index = 0; failure_index < SXML_TEST_COUNT(known_failures);
         failure_index += 1) {
        if (strcmp(known_failures[failure_index].case_name, case_name) == 0) {
            return true;
        }
    }
    return false;
}

/* Counts one outcome; a failure or a fixed case not as listed is
   reported. */
typedef struct case_outcome {
    const char *case_name;
    bool has_passed;
    sxml_byte_view expected;
    sxml_byte_view actual;
} case_outcome;

static void count_outcome(suite_counts *counts, const case_outcome *outcome) {
    const bool is_known = is_known_failure(outcome->case_name);
    if (outcome->has_passed) {
        counts->passed_count += 1;
        if (is_known) {
            counts->fixed_count += 1;
            printf("listed as failing, but passes: %s\n", outcome->case_name);
        }
        return;
    }
    counts->failed_count += 1;
    if (!is_known) {
        counts->unexpected_count += 1;
        printf("fails: %s\n--- expected\n%.*s\n--- actual\n%.*s\n---\n", outcome->case_name,
               (int)outcome->expected.byte_count, (const char *)outcome->expected.bytes,
               (int)outcome->actual.byte_count, (const char *)outcome->actual.bytes);
    }
}

/* ------------------------------------------------- Tree construction */

/* One test of a .dat file; views into the file. */
typedef struct tree_case {
    sxml_byte_view data;
    sxml_byte_view document;
    size_t error_count;
    bool is_fragment;
    bool is_script_on;
} tree_case;

/* Reads the test in block, from "#data" or "#description" to the end of
   its "#document". Returns false for a block that is not a test. */
static bool read_tree_case(sxml_byte_view block, tree_case *test_case) {
    *test_case = (tree_case){.data = {.bytes = NULL, .byte_count = 0},
                             .document = {.bytes = NULL, .byte_count = 0},
                             .error_count = 0,
                             .is_fragment = false,
                             .is_script_on = false};
    const size_t data_header =
        starts_with(block, TEXT("#data\n"))
            ? 0
            : find_text((text_search){.haystack = block, .start = 0}, TEXT("\n#data\n"));
    if (data_header == SXML_TEST_NO_VALUE) {
        return false;
    }
    const size_t data_start = data_header + (data_header == 0 ? 6 : 7);
    const size_t errors_header =
        find_text((text_search){.haystack = block, .start = data_start - 1}, TEXT("\n#errors\n"));
    if (errors_header == SXML_TEST_NO_VALUE) {
        return false;
    }
    test_case->data =
        (sxml_byte_view){.bytes = block.bytes + data_start,
                         .byte_count = errors_header > data_start ? errors_header - data_start : 0};
    size_t line_start = errors_header + 9;
    while (line_start < block.byte_count) {
        size_t line_end = line_start;
        while (line_end < block.byte_count && block.bytes[line_end] != '\n') {
            line_end += 1;
        }
        const sxml_byte_view line = {.bytes = block.bytes + line_start,
                                     .byte_count = line_end - line_start};
        line_start = line_end + 1;
        if (sxml_byte_view_equals(line, TEXT("#document"))) {
            test_case->document = (sxml_byte_view){
                .bytes =
                    block.bytes + (line_start < block.byte_count ? line_start : block.byte_count),
                .byte_count = line_start < block.byte_count ? block.byte_count - line_start : 0};
            return true;
        }
        if (sxml_byte_view_equals(line, TEXT("#document-fragment"))) {
            test_case->is_fragment = true;
        } else if (sxml_byte_view_equals(line, TEXT("#script-on"))) {
            test_case->is_script_on = true;
        } else if (line.byte_count > 0 && line.bytes[0] != '#') {
            test_case->error_count += 1;
        }
    }
    return false;
}

/* The expected document without its DOCTYPE lines. */
static sxml_status expected_document(sxml_byte_view document, sxml_xml_buffer *expected) {
    size_t line_start = 0;
    sxml_status status = SXML_STATUS_OK;
    bool is_first = true;
    while (status == SXML_STATUS_OK && line_start < document.byte_count) {
        size_t line_end = line_start;
        while (line_end < document.byte_count && document.bytes[line_end] != '\n') {
            line_end += 1;
        }
        const sxml_byte_view line = {.bytes = document.bytes + line_start,
                                     .byte_count = line_end - line_start};
        line_start = line_end + 1;
        if (starts_with(line, TEXT("| <!DOCTYPE "))) {
            continue;
        }
        if (!is_first) {
            status = sxml_xml_buffer_append_byte(expected, '\n');
        }
        is_first = false;
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(expected, line);
        }
    }
    return status;
}

/* What writing a working tree needs. */
typedef struct tree_writer {
    const sxml_html_working_tree *tree;
    sxml_xml_buffer *output;
    /* Attribute display names and values, for sorting. */
    sxml_xml_buffer attribute_text;
} tree_writer;

static sxml_byte_view tree_text(const tree_writer *writer, sxml_byte_range range) {
    return sxml_xml_buffer_view(&writer->tree->text, range);
}

static bool is_template(const tree_writer *writer, const sxml_html_node *node) {
    return node->node_kind == SXML_HTML_NODE_KIND_ELEMENT &&
           node->element_namespace == SXML_HTML_NAMESPACE_HTML &&
           sxml_byte_view_equals(tree_text(writer, node->name), TEXT("template"));
}

/* The depth of a node below the document: two per template ancestor,
   whose children are written below "content". */
static size_t node_depth(const tree_writer *writer, uint32_t node_number) {
    size_t depth = 0;
    const sxml_html_node *node = sxml_html_working_tree_node(writer->tree, node_number);
    while (node->parent != 0 && node->parent != SXML_HTML_NO_NODE) {
        const sxml_html_node *parent = sxml_html_working_tree_node(writer->tree, node->parent);
        depth += is_template(writer, parent) ? 2 : 1;
        node = parent;
    }
    return depth;
}

/* Starts a line at depth: "| " and two spaces per level. */
static sxml_status start_line(tree_writer *writer, size_t depth) {
    sxml_status status = SXML_STATUS_OK;
    if (writer->output->byte_count > 0) {
        status = sxml_xml_buffer_append_byte(writer->output, '\n');
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(writer->output, TEXT("| "));
    }
    for (size_t level = 0; status == SXML_STATUS_OK && level < depth; level += 1) {
        status = sxml_xml_buffer_append(writer->output, TEXT("  "));
    }
    return status;
}

/* One attribute line's text, in writer->attribute_text. */
typedef struct attribute_line {
    sxml_byte_range text_range;
} attribute_line;

static bool range_precedes(const tree_writer *writer, sxml_byte_range first,
                           sxml_byte_range second) {
    const sxml_byte_view first_text = sxml_xml_buffer_view(&writer->attribute_text, first);
    const sxml_byte_view second_text = sxml_xml_buffer_view(&writer->attribute_text, second);
    const size_t common = first_text.byte_count < second_text.byte_count ? first_text.byte_count
                                                                         : second_text.byte_count;
    const int order = memcmp(first_text.bytes, second_text.bytes, common);
    return order < 0 || (order == 0 && first_text.byte_count < second_text.byte_count);
}

/* Writes an element's attributes at depth, sorted by their lines (whose
   order is that of the attribute name strings: '=' sorts before every
   name character). */
static sxml_status write_attributes(tree_writer *writer, const sxml_html_node *node, size_t depth) {
    static const sxml_byte_view designators[] = {
        SXML_BYTE_VIEW_INITIALIZER(""), SXML_BYTE_VIEW_INITIALIZER("xlink "),
        SXML_BYTE_VIEW_INITIALIZER("xml "), SXML_BYTE_VIEW_INITIALIZER("xmlns ")};
    attribute_line lines[256];
    const size_t line_count = node->attribute_count < 256 ? node->attribute_count : 256;
    writer->attribute_text.byte_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t attribute_index = 0; status == SXML_STATUS_OK && attribute_index < line_count;
         attribute_index += 1) {
        const sxml_html_attribute *attribute = sxml_xml_array_at(
            &writer->tree->attributes, (size_t)node->first_attribute + attribute_index);
        const size_t start = writer->attribute_text.byte_count;
        status = sxml_xml_buffer_append(&writer->attribute_text,
                                        designators[attribute->attribute_namespace]);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&writer->attribute_text,
                                            tree_text(writer, attribute->attribute_name));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&writer->attribute_text, TEXT("=\""));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&writer->attribute_text,
                                            tree_text(writer, attribute->attribute_value));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(&writer->attribute_text, '"');
        }
        lines[attribute_index] = (attribute_line){
            .text_range = {.byte_offset = start,
                           .byte_count = writer->attribute_text.byte_count - start}};
    }
    /* Insertion sort: few attributes. */
    for (size_t sorted_count = 1; sorted_count < line_count; sorted_count += 1) {
        const attribute_line moving = lines[sorted_count];
        size_t position = sorted_count;
        while (position > 0 &&
               range_precedes(writer, moving.text_range, lines[position - 1].text_range)) {
            lines[position] = lines[position - 1];
            position -= 1;
        }
        lines[position] = moving;
    }
    for (size_t line_index = 0; status == SXML_STATUS_OK && line_index < line_count;
         line_index += 1) {
        status = start_line(writer, depth);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(
                writer->output,
                sxml_xml_buffer_view(&writer->attribute_text, lines[line_index].text_range));
        }
    }
    return status;
}

/* Writes one node's lines (an element's attributes and "content" too). */
static sxml_status write_node(tree_writer *writer, uint32_t node_number) {
    static const sxml_byte_view element_designators[] = {SXML_BYTE_VIEW_INITIALIZER(""),
                                                         SXML_BYTE_VIEW_INITIALIZER("math "),
                                                         SXML_BYTE_VIEW_INITIALIZER("svg ")};
    const sxml_html_node *node = sxml_html_working_tree_node(writer->tree, node_number);
    const size_t depth = node_depth(writer, node_number);
    sxml_xml_buffer *output = writer->output;
    sxml_status status = start_line(writer, depth);
    switch (node->node_kind) {
    case SXML_HTML_NODE_KIND_ELEMENT:
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(output, '<');
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, element_designators[node->element_namespace]);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, tree_text(writer, node->name));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(output, '>');
        }
        if (status == SXML_STATUS_OK) {
            status = write_attributes(writer, node, depth + 1);
        }
        if (status == SXML_STATUS_OK && is_template(writer, node)) {
            status = start_line(writer, depth + 1);
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append(output, TEXT("content"));
            }
        }
        return status;
    case SXML_HTML_NODE_KIND_TEXT:
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(output, '"');
        }
        for (uint32_t segment_number = node->first_segment;
             status == SXML_STATUS_OK && segment_number != SXML_HTML_NO_NODE;) {
            const sxml_html_text_segment *segment =
                sxml_xml_array_at(&writer->tree->segments, segment_number);
            status = sxml_xml_buffer_append(output, tree_text(writer, segment->text_range));
            segment_number = segment->next_segment;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(output, '"');
        }
        return status;
    case SXML_HTML_NODE_KIND_COMMENT:
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, TEXT("<!-- "));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, tree_text(writer, node->data));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, TEXT(" -->"));
        }
        return status;
    case SXML_HTML_NODE_KIND_PROCESSING_INSTRUCTION:
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, TEXT("<?"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, tree_text(writer, node->name));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(output, ' ');
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, tree_text(writer, node->data));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(output, TEXT("?>"));
        }
        return status;
    case SXML_HTML_NODE_KIND_DOCUMENT:
        break;
    }
    return status;
}

/* Writes the working tree in document order by its links (no recursion,
   D71). */
static sxml_status write_tree(tree_writer *writer) {
    const sxml_html_working_tree *tree = writer->tree;
    uint32_t node_number = sxml_html_working_tree_node(tree, 0)->first_child;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && node_number != SXML_HTML_NO_NODE) {
        status = write_node(writer, node_number);
        const sxml_html_node *node = sxml_html_working_tree_node(tree, node_number);
        if (node->first_child != SXML_HTML_NO_NODE) {
            node_number = node->first_child;
            continue;
        }
        while (node_number != SXML_HTML_NO_NODE) {
            const sxml_html_node *current = sxml_html_working_tree_node(tree, node_number);
            if (current->next_sibling != SXML_HTML_NO_NODE) {
                node_number = current->next_sibling;
                break;
            }
            node_number = current->parent == 0 ? SXML_HTML_NO_NODE : current->parent;
        }
    }
    return status;
}

/* Parses data as a document and writes its working tree to output. */
typedef struct tree_result {
    sxml_xml_buffer *output;
    bool has_parse_error;
} tree_result;

static sxml_status parse_and_write(sxml_byte_view data, tree_result *result) {
    const sxml_allocator allocator = result->output->allocator;
    sxml_xml_buffer stream = sxml_xml_buffer_empty(allocator);
    bool has_stream_error = false;
    sxml_status status = make_stream(&(stream_request){.input = data,
                                                       .prefix = {.bytes = NULL, .byte_count = 0},
                                                       .has_parse_error = &has_stream_error},
                                     &stream);
    sxml_html_working_tree tree;
    if (status == SXML_STATUS_OK) {
        status = sxml_html_working_tree_initialize(&tree, allocator);
        sxml_html_parse_outcome outcome = {.has_parse_error = false};
        if (status == SXML_STATUS_OK) {
            status =
                sxml_html_construct_tree(&(sxml_html_parse_request){.input = view_of(&stream),
                                                                    .stops_at_parse_error = false},
                                         &tree, &outcome);
        }
        tree_writer writer = {.tree = &tree,
                              .output = result->output,
                              .attribute_text = sxml_xml_buffer_empty(allocator)};
        if (status == SXML_STATUS_OK) {
            status = write_tree(&writer);
        }
        result->has_parse_error = has_stream_error || outcome.has_parse_error;
        sxml_xml_buffer_release(&writer.attribute_text);
        sxml_html_working_tree_finalize(&tree);
    }
    sxml_xml_buffer_release(&stream);
    return status;
}

/* Runs one test of file_name, the test_number-th. */
typedef struct tree_run {
    sxml_test_state *state;
    suite_counts *counts;
    const char *file_name;
    size_t test_number;
} tree_run;

static void run_tree_case(const tree_run *run, const tree_case *test_case) {
    if (test_case->is_fragment || test_case->is_script_on) {
        run->counts->left_out_count += 1;
        return;
    }
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_xml_buffer expected = sxml_xml_buffer_empty(allocator);
    sxml_xml_buffer actual = sxml_xml_buffer_empty(allocator);
    tree_result result = {.output = &actual, .has_parse_error = false};
    sxml_status status = expected_document(test_case->document, &expected);
    if (status == SXML_STATUS_OK) {
        status = parse_and_write(test_case->data, &result);
    }
    SXML_TEST_EXPECT(run->state, status == SXML_STATUS_OK);
    char case_name[SXML_TEST_NAME_BYTE_COUNT];
    (void)snprintf(case_name, sizeof(case_name), "%s:%zu", run->file_name, run->test_number);
    count_outcome(
        run->counts,
        &(case_outcome){.case_name = case_name,
                        .has_passed = status == SXML_STATUS_OK &&
                                      sxml_byte_view_equals(view_of(&expected), view_of(&actual)),
                        .expected = view_of(&expected),
                        .actual = view_of(&actual)});
    if (result.has_parse_error == (test_case->error_count > 0)) {
        run->counts->error_agreement_count += 1;
    } else {
        run->counts->error_disagreement_count += 1;
        run->counts->missed_error_count += result.has_parse_error ? 0 : 1;
    }
    sxml_xml_buffer_release(&actual);
    sxml_xml_buffer_release(&expected);
}

/* Runs every test of one .dat file. */
static void run_tree_file(sxml_test_state *state, const char *file_name, suite_counts *counts) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    suite_directory(directory);
    char path[SXML_TEST_PATH_BYTE_COUNT + 64];
    (void)snprintf(path, sizeof(path), "%stree-construction/%s", directory, file_name);
    sxml_xml_buffer contents = sxml_xml_buffer_empty(sxml_default_allocator());
    SXML_TEST_EXPECT(state, read_file(path, &contents));
    const sxml_byte_view file = view_of(&contents);
    size_t block_start = 0;
    size_t test_number = 1;
    while (block_start < file.byte_count) {
        /* Tests are separated by an empty line before "#data" or
           "#description". */
        size_t block_end =
            find_text((text_search){.haystack = file, .start = block_start}, TEXT("\n\n#data\n"));
        const size_t described = find_text((text_search){.haystack = file, .start = block_start},
                                           TEXT("\n\n#description\n"));
        if (described < block_end) {
            block_end = described;
        }
        const size_t next_start = block_end == SXML_TEST_NO_VALUE ? file.byte_count : block_end + 2;
        if (block_end == SXML_TEST_NO_VALUE) {
            block_end = file.byte_count;
            while (block_end > block_start && file.bytes[block_end - 1] == '\n') {
                block_end -= 1;
            }
        }
        tree_case test_case;
        if (read_tree_case(slice(file, (sxml_byte_range){.byte_offset = block_start,
                                                         .byte_count = block_end - block_start}),
                           &test_case)) {
            run_tree_case(&(tree_run){.state = state,
                                      .counts = counts,
                                      .file_name = file_name,
                                      .test_number = test_number},
                          &test_case);
            test_number += 1;
        }
        block_start = next_start;
    }
    sxml_xml_buffer_release(&contents);
}

static const char *const tree_file_names[] = {
    "adoption01.dat",
    "adoption02.dat",
    "blocks.dat",
    "comments01.dat",
    "doctype01.dat",
    "domjs-unsafe.dat",
    "entities01.dat",
    "entities02.dat",
    "foreign-fragment.dat",
    "html5test-com.dat",
    "inbody01.dat",
    "isindex.dat",
    "main-element.dat",
    "math.dat",
    "menuitem-element.dat",
    "namespace-sensitivity.dat",
    "noscript01.dat",
    "pending-spec-changes-plain-text-unsafe.dat",
    "pending-spec-changes.dat",
    "plain-text-unsafe.dat",
    "processing-instructions.dat",
    "quirks01.dat",
    "ruby.dat",
    "scriptdata01.dat",
    "scripted_adoption01.dat",
    "scripted_ark.dat",
    "scripted_foster01.dat",
    "scripted_webkit01.dat",
    "search-element.dat",
    "svg.dat",
    "tables01.dat",
    "template.dat",
    "tests1.dat",
    "tests10.dat",
    "tests11.dat",
    "tests12.dat",
    "tests14.dat",
    "tests15.dat",
    "tests16.dat",
    "tests17.dat",
    "tests18.dat",
    "tests19.dat",
    "tests2.dat",
    "tests20.dat",
    "tests21.dat",
    "tests22.dat",
    "tests23.dat",
    "tests24.dat",
    "tests25.dat",
    "tests26.dat",
    "tests3.dat",
    "tests4.dat",
    "tests5.dat",
    "tests6.dat",
    "tests7.dat",
    "tests8.dat",
    "tests9.dat",
    "tests_innerHTML_1.dat",
    "tricky01.dat",
    "void-in-phrasing.dat",
    "webkit01.dat",
    "webkit02.dat",
};

/* True when the suite can be read; says so otherwise. */
static bool suite_is_reachable(const char *probe_name) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    suite_directory(directory);
    char path[SXML_TEST_PATH_BYTE_COUNT + 64];
    (void)snprintf(path, sizeof(path), "%s%s", directory, probe_name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        printf("skipped: the html5lib tests are not reachable at %s\n", path);
        return false;
    }
    (void)fclose(file);
    return true;
}

static void report_counts(const char *suite_name, const suite_counts *counts) {
    printf("%s: %zu passed, %zu failed (%zu not listed), %zu left out; whether "
           "there is a "
           "parse error: agrees in %zu, differs in %zu (%zu missed)\n",
           suite_name, counts->passed_count, counts->failed_count, counts->unexpected_count,
           counts->left_out_count, counts->error_agreement_count, counts->error_disagreement_count,
           counts->missed_error_count);
}

static void test_tree_construction(sxml_test_state *state) {
    if (!suite_is_reachable("tree-construction/tests1.dat")) {
        return;
    }
    suite_counts counts;
    memset(&counts, 0, sizeof(counts));
    for (size_t file_index = 0; file_index < SXML_TEST_COUNT(tree_file_names); file_index += 1) {
        run_tree_file(state, tree_file_names[file_index], &counts);
    }
    report_counts("tree construction", &counts);
    SXML_TEST_EXPECT(state, counts.unexpected_count == 0);
    SXML_TEST_EXPECT(state, counts.missed_error_count == 0);
    SXML_TEST_EXPECT(state, counts.fixed_count == 0);
    SXML_TEST_EXPECT(state, counts.passed_count > 1700);
}

/* ------------------------------------------------------------- JSON */

/* A JSON value of a tokenizer test file (RFC 8259), in a value table:
   the reader keeps no stack, a container's parent link is enough. */
typedef enum json_kind {
    SXML_TEST_JSON_KIND_NULL,
    SXML_TEST_JSON_KIND_TRUE,
    SXML_TEST_JSON_KIND_FALSE,
    SXML_TEST_JSON_KIND_NUMBER,
    SXML_TEST_JSON_KIND_STRING,
    SXML_TEST_JSON_KIND_ARRAY,
    SXML_TEST_JSON_KIND_OBJECT,
} json_kind;

typedef struct json_value {
    json_kind value_kind;
    size_t parent;
    size_t first_child;
    size_t last_child;
    size_t next_sibling;
    size_t child_count;
    /* A string's decoded text and a member's key, in the document's
       text. */
    sxml_byte_range string_range;
    sxml_byte_range key_range;
    /* An escape of a lone surrogate was read (written as U+FFFD). */
    bool has_lone_surrogate;
} json_value;

typedef struct json_document {
    sxml_xml_array values;
    sxml_xml_buffer text;
} json_document;

static json_value *json_at(const json_document *document, size_t value_index) {
    return sxml_xml_array_at(&document->values, value_index);
}

static size_t skip_json_space(sxml_byte_view input, size_t offset) {
    while (offset < input.byte_count &&
           (input.bytes[offset] == ' ' || input.bytes[offset] == '\n' ||
            input.bytes[offset] == '\r' || input.bytes[offset] == '\t')) {
        offset += 1;
    }
    return offset;
}

static uint32_t hexadecimal_value(sxml_byte_view input, size_t offset) {
    uint32_t value = 0;
    for (size_t digit_index = 0; digit_index < 4 && offset + digit_index < input.byte_count;
         digit_index += 1) {
        const uint8_t digit = input.bytes[offset + digit_index];
        const uint32_t digit_value = digit >= '0' && digit <= '9'   ? (uint32_t)(digit - '0')
                                     : digit >= 'a' && digit <= 'f' ? (uint32_t)(digit - 'a' + 10)
                                     : digit >= 'A' && digit <= 'F' ? (uint32_t)(digit - 'A' + 10)
                                                                    : 0;
        value = (value << 4U) | digit_value;
    }
    return value;
}

/* Where an escape reader is and what it found. */
typedef struct escape_reader {
    sxml_byte_view input;
    size_t offset;
    bool has_lone_surrogate;
} escape_reader;

/* Reads "\uXXXX" at reader->offset (after the backslash and 'u'),
   joining a surrogate pair, and appends the codepoint. */
static sxml_status append_unicode_escape(escape_reader *reader, sxml_xml_buffer *text) {
    const sxml_byte_view input = reader->input;
    uint32_t codepoint = hexadecimal_value(input, reader->offset);
    reader->offset += 4;
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF && reader->offset + 6 <= input.byte_count &&
        input.bytes[reader->offset] == '\\' && input.bytes[reader->offset + 1] == 'u') {
        const uint32_t trail = hexadecimal_value(input, reader->offset + 2);
        if (trail >= 0xDC00 && trail <= 0xDFFF) {
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (trail - 0xDC00U);
            reader->offset += 6;
        }
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        reader->has_lone_surrogate = true;
        codepoint = 0xFFFD;
    }
    return sxml_xml_buffer_append_codepoint(text, codepoint);
}

/* Reads the string at reader->offset (at its quote) into text. */
static sxml_status read_json_string(escape_reader *reader, sxml_xml_buffer *text) {
    const sxml_byte_view input = reader->input;
    reader->offset += 1;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && reader->offset < input.byte_count &&
           input.bytes[reader->offset] != '"') {
        const uint8_t byte = input.bytes[reader->offset];
        if (byte != '\\' || reader->offset + 1 >= input.byte_count) {
            status = sxml_xml_buffer_append_byte(text, byte);
            reader->offset += 1;
            continue;
        }
        const uint8_t escaped = input.bytes[reader->offset + 1];
        reader->offset += 2;
        switch (escaped) {
        case 'u':
            status = append_unicode_escape(reader, text);
            break;
        case 'b':
            status = sxml_xml_buffer_append_byte(text, '\b');
            break;
        case 'f':
            status = sxml_xml_buffer_append_byte(text, '\f');
            break;
        case 'n':
            status = sxml_xml_buffer_append_byte(text, '\n');
            break;
        case 'r':
            status = sxml_xml_buffer_append_byte(text, '\r');
            break;
        case 't':
            status = sxml_xml_buffer_append_byte(text, '\t');
            break;
        default:
            status = sxml_xml_buffer_append_byte(text, escaped);
            break;
        }
    }
    reader->offset += 1;
    return status;
}

/* Adds a value below container (SXML_TEST_NO_VALUE for the root). */
typedef struct json_addition {
    size_t container;
    json_kind value_kind;
    sxml_byte_range key_range;
} json_addition;

static sxml_status add_json_value(json_document *document, const json_addition *addition,
                                  size_t *value_index) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&document->values, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *value_index = document->values.element_count - 1;
    json_value *value = element;
    *value = (json_value){.value_kind = addition->value_kind,
                          .parent = addition->container,
                          .first_child = SXML_TEST_NO_VALUE,
                          .last_child = SXML_TEST_NO_VALUE,
                          .next_sibling = SXML_TEST_NO_VALUE,
                          .child_count = 0,
                          .string_range = {.byte_offset = 0, .byte_count = 0},
                          .key_range = addition->key_range,
                          .has_lone_surrogate = false};
    if (addition->container != SXML_TEST_NO_VALUE) {
        json_value *container = json_at(document, addition->container);
        if (container->last_child == SXML_TEST_NO_VALUE) {
            container->first_child = *value_index;
        } else {
            json_at(document, container->last_child)->next_sibling = *value_index;
        }
        container->last_child = *value_index;
        container->child_count += 1;
    }
    return SXML_STATUS_OK;
}

/* Reads a JSON text into document (value 0 the root). Lenient: the files
   are trusted test data. */
static sxml_status read_json(sxml_byte_view input, json_document *document) {
    size_t container = SXML_TEST_NO_VALUE;
    sxml_byte_range key_range = {.byte_offset = 0, .byte_count = 0};
    bool has_key = false;
    bool has_root = false;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset < input.byte_count) {
        offset = skip_json_space(input, offset);
        if (offset >= input.byte_count || (has_root && container == SXML_TEST_NO_VALUE)) {
            break;
        }
        const uint8_t byte = input.bytes[offset];
        if (byte == ']' || byte == '}') {
            container = json_at(document, container)->parent;
            offset += 1;
            continue;
        }
        if (byte == ',' || byte == ':') {
            offset += 1;
            continue;
        }
        const bool is_in_object =
            container != SXML_TEST_NO_VALUE &&
            json_at(document, container)->value_kind == SXML_TEST_JSON_KIND_OBJECT;
        if (is_in_object && !has_key) {
            escape_reader reader = {.input = input, .offset = offset, .has_lone_surrogate = false};
            const size_t start = document->text.byte_count;
            status = read_json_string(&reader, &document->text);
            key_range = (sxml_byte_range){.byte_offset = start,
                                          .byte_count = document->text.byte_count - start};
            has_key = true;
            offset = reader.offset;
            continue;
        }
        json_kind value_kind = SXML_TEST_JSON_KIND_NUMBER;
        switch (byte) {
        case '{':
            value_kind = SXML_TEST_JSON_KIND_OBJECT;
            break;
        case '[':
            value_kind = SXML_TEST_JSON_KIND_ARRAY;
            break;
        case '"':
            value_kind = SXML_TEST_JSON_KIND_STRING;
            break;
        case 't':
            value_kind = SXML_TEST_JSON_KIND_TRUE;
            break;
        case 'f':
            value_kind = SXML_TEST_JSON_KIND_FALSE;
            break;
        case 'n':
            value_kind = SXML_TEST_JSON_KIND_NULL;
            break;
        default:
            break;
        }
        size_t value_index = 0;
        status = add_json_value(
            document,
            &(json_addition){.container = container,
                             .value_kind = value_kind,
                             .key_range =
                                 has_key ? key_range
                                         : (sxml_byte_range){.byte_offset = 0, .byte_count = 0}},
            &value_index);
        has_key = false;
        has_root = true;
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (value_kind == SXML_TEST_JSON_KIND_OBJECT || value_kind == SXML_TEST_JSON_KIND_ARRAY) {
            container = value_index;
            offset += 1;
        } else if (value_kind == SXML_TEST_JSON_KIND_STRING) {
            escape_reader reader = {.input = input, .offset = offset, .has_lone_surrogate = false};
            const size_t start = document->text.byte_count;
            status = read_json_string(&reader, &document->text);
            json_value *value = json_at(document, value_index);
            value->string_range = (sxml_byte_range){
                .byte_offset = start, .byte_count = document->text.byte_count - start};
            value->has_lone_surrogate = reader.has_lone_surrogate;
            offset = reader.offset;
        } else {
            while (offset < input.byte_count && input.bytes[offset] != ',' &&
                   input.bytes[offset] != ']' && input.bytes[offset] != '}' &&
                   input.bytes[offset] != ' ' && input.bytes[offset] != '\n') {
                offset += 1;
            }
        }
    }
    return status;
}

static sxml_byte_view json_string(const json_document *document, size_t value_index) {
    return sxml_xml_buffer_view(&document->text, json_at(document, value_index)->string_range);
}

/* The member of object named key, or SXML_TEST_NO_VALUE. */
typedef struct json_member_request {
    size_t object;
    sxml_byte_view key;
} json_member_request;

static size_t json_member(const json_document *document, json_member_request request) {
    if (request.object == SXML_TEST_NO_VALUE) {
        return SXML_TEST_NO_VALUE;
    }
    for (size_t member = json_at(document, request.object)->first_child;
         member != SXML_TEST_NO_VALUE; member = json_at(document, member)->next_sibling) {
        if (sxml_byte_view_equals(
                sxml_xml_buffer_view(&document->text, json_at(document, member)->key_range),
                request.key)) {
            return member;
        }
    }
    return SXML_TEST_NO_VALUE;
}

/* The element_number-th element (from 0) of an array, or SXML_TEST_NO_VALUE. */
static size_t json_element(const json_document *document, size_t array, size_t element_number) {
    size_t element =
        array == SXML_TEST_NO_VALUE ? SXML_TEST_NO_VALUE : json_at(document, array)->first_child;
    for (size_t skipped = 0; element != SXML_TEST_NO_VALUE && skipped < element_number;
         skipped += 1) {
        element = json_at(document, element)->next_sibling;
    }
    return element;
}

/* ---------------------------------------------------------- Tokenizer */

/* Appends text, unescaping "\uXXXX" when the test is doubleEscaped. */
typedef struct test_text {
    sxml_byte_view text;
    bool is_double_escaped;
} test_text;

/* Where unescaped text goes, and the flag set when it held a lone
   surrogate. */
typedef struct text_destination {
    sxml_xml_buffer *output;
    bool *has_lone_surrogate;
} text_destination;

static sxml_status append_test_text(const test_text *source, const text_destination *destination) {
    sxml_xml_buffer *output = destination->output;
    if (!source->is_double_escaped) {
        return sxml_xml_buffer_append(output, source->text);
    }
    escape_reader reader = {.input = source->text, .offset = 0, .has_lone_surrogate = false};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && reader.offset < source->text.byte_count) {
        if (reader.offset + 6 <= source->text.byte_count &&
            source->text.bytes[reader.offset] == '\\' &&
            source->text.bytes[reader.offset + 1] == 'u') {
            reader.offset += 2;
            status = append_unicode_escape(&reader, output);
            continue;
        }
        status = sxml_xml_buffer_append_byte(output, source->text.bytes[reader.offset]);
        reader.offset += 1;
    }
    *destination->has_lone_surrogate =
        *destination->has_lone_surrogate || reader.has_lone_surrogate;
    return status;
}

/* Token lists are written as text: tokens separated by 0x01, fields by
   0x02, attributes sorted; consecutive characters joined. */
enum { SXML_TEST_TOKEN_SEPARATOR = 0x01, SXML_TEST_FIELD_SEPARATOR = 0x02 };

/* One attribute of a token, as ranges of a scratch buffer. */
typedef struct token_attribute {
    sxml_byte_range attribute_name;
    sxml_byte_range attribute_value;
} token_attribute;

/* A token list being written. */
typedef struct token_writer {
    sxml_xml_buffer *output;
    /* The last token written was characters. */
    bool follows_characters;
    bool has_lone_surrogate;
    /* The tokenizer or the input stream found a parse error. */
    bool has_parse_error;
} token_writer;

static sxml_status start_token(token_writer *writer, uint8_t kind_letter) {
    writer->follows_characters = kind_letter == 'C';
    sxml_status status = sxml_xml_buffer_append_byte(writer->output, SXML_TEST_TOKEN_SEPARATOR);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append_byte(writer->output, kind_letter);
    }
    return status;
}

static sxml_status write_field(token_writer *writer, sxml_byte_view field) {
    sxml_status status = sxml_xml_buffer_append_byte(writer->output, SXML_TEST_FIELD_SEPARATOR);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(writer->output, field);
    }
    return status;
}

/* Writes characters, joined with the characters before them. */
static sxml_status write_characters(token_writer *writer, const test_text *characters) {
    sxml_status status = SXML_STATUS_OK;
    if (!writer->follows_characters) {
        status = start_token(writer, 'C');
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(writer->output, SXML_TEST_FIELD_SEPARATOR);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append_test_text(
            characters, &(text_destination){.output = writer->output,
                                            .has_lone_surrogate = &writer->has_lone_surrogate});
    }
    return status;
}

/* The attributes of a start tag, to be sorted and written. */
typedef struct attribute_list {
    token_attribute attributes[64];
    size_t attribute_count;
    sxml_xml_buffer *text;
} attribute_list;

static bool attribute_precedes(const attribute_list *list, size_t first, size_t second) {
    const sxml_byte_view first_name =
        sxml_xml_buffer_view(list->text, list->attributes[first].attribute_name);
    const sxml_byte_view second_name =
        sxml_xml_buffer_view(list->text, list->attributes[second].attribute_name);
    const size_t common = first_name.byte_count < second_name.byte_count ? first_name.byte_count
                                                                         : second_name.byte_count;
    const int order = memcmp(first_name.bytes, second_name.bytes, common);
    return order < 0 || (order == 0 && first_name.byte_count < second_name.byte_count);
}

static sxml_status write_attribute_list(token_writer *writer, attribute_list *list) {
    for (size_t sorted_count = 1; sorted_count < list->attribute_count; sorted_count += 1) {
        size_t position = sorted_count;
        while (position > 0 && attribute_precedes(list, position, position - 1)) {
            const token_attribute swapped = list->attributes[position];
            list->attributes[position] = list->attributes[position - 1];
            list->attributes[position - 1] = swapped;
            position -= 1;
        }
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < list->attribute_count;
         attribute_index += 1) {
        status = write_field(
            writer,
            sxml_xml_buffer_view(list->text, list->attributes[attribute_index].attribute_name));
        if (status == SXML_STATUS_OK) {
            status = write_field(
                writer, sxml_xml_buffer_view(list->text,
                                             list->attributes[attribute_index].attribute_value));
        }
    }
    return status;
}

/* A test's expected output, written. */
typedef struct expected_request {
    const json_document *document;
    size_t output;
    bool is_double_escaped;
} expected_request;

/* Appends a JSON string to text and returns its range there. */
typedef struct string_copy {
    const json_document *document;
    size_t value_index;
    bool is_double_escaped;
} string_copy;

static sxml_status copy_string(const string_copy *copy, sxml_xml_buffer *text,
                               sxml_byte_range *range) {
    const size_t start = text->byte_count;
    bool has_lone_surrogate = false;
    const sxml_status status = append_test_text(
        &(test_text){.text = json_string(copy->document, copy->value_index),
                     .is_double_escaped = copy->is_double_escaped},
        &(text_destination){.output = text, .has_lone_surrogate = &has_lone_surrogate});
    *range = (sxml_byte_range){.byte_offset = start, .byte_count = text->byte_count - start};
    return status;
}

/* Writes the fields of one expected token after its kind letter. */
static sxml_status write_expected_fields(token_writer *writer, const expected_request *request,
                                         size_t token) {
    const json_document *document = request->document;
    const sxml_byte_view kind = json_string(document, json_element(document, token, 0));
    if (sxml_byte_view_equals(kind, TEXT("Character"))) {
        return write_characters(
            writer, &(test_text){.text = json_string(document, json_element(document, token, 1)),
                                 .is_double_escaped = request->is_double_escaped});
    }
    if (sxml_byte_view_equals(kind, TEXT("ProcessingInstruction"))) {
        /* 13.2.5.72 to 13.2.5.76: a target and data. */
        sxml_status status = start_token(writer, 'P');
        for (size_t field_index = 1; status == SXML_STATUS_OK && field_index < 3;
             field_index += 1) {
            status = sxml_xml_buffer_append_byte(writer->output, SXML_TEST_FIELD_SEPARATOR);
            if (status == SXML_STATUS_OK) {
                status = append_test_text(
                    &(test_text){
                        .text = json_string(document, json_element(document, token, field_index)),
                        .is_double_escaped = request->is_double_escaped},
                    &(text_destination){.output = writer->output,
                                        .has_lone_surrogate = &writer->has_lone_surrogate});
            }
        }
        return status;
    }
    const bool is_start = sxml_byte_view_equals(kind, TEXT("StartTag"));
    const bool is_doctype = sxml_byte_view_equals(kind, TEXT("DOCTYPE"));
    const uint8_t letter = is_start                                       ? 'S'
                           : is_doctype                                   ? 'D'
                           : sxml_byte_view_equals(kind, TEXT("EndTag"))  ? 'E'
                           : sxml_byte_view_equals(kind, TEXT("Comment")) ? 'M'
                                                                          : '?';
    sxml_status status = start_token(writer, letter);
    const size_t field_count = is_doctype ? 4 : 2;
    sxml_xml_buffer field = sxml_xml_buffer_empty(writer->output->allocator);
    for (size_t field_index = 1; status == SXML_STATUS_OK && field_index < field_count;
         field_index += 1) {
        const size_t value = json_element(document, token, field_index);
        field.byte_count = 0;
        if (value != SXML_TEST_NO_VALUE &&
            json_at(document, value)->value_kind == SXML_TEST_JSON_KIND_STRING) {
            status = sxml_xml_buffer_append_byte(&field, '"');
            if (status == SXML_STATUS_OK) {
                status = append_test_text(
                    &(test_text){.text = json_string(document, value),
                                 .is_double_escaped = request->is_double_escaped},
                    &(text_destination){.output = &field,
                                        .has_lone_surrogate = &writer->has_lone_surrogate});
            }
        } else {
            status = sxml_xml_buffer_append(&field, TEXT("null"));
        }
        if (status == SXML_STATUS_OK) {
            status = write_field(writer, view_of(&field));
        }
    }
    sxml_xml_buffer_release(&field);
    if (status == SXML_STATUS_OK && is_doctype) {
        const size_t correctness = json_element(document, token, 4);
        status = write_field(writer, correctness != SXML_TEST_NO_VALUE &&
                                             json_at(document, correctness)->value_kind ==
                                                 SXML_TEST_JSON_KIND_TRUE
                                         ? TEXT("correct")
                                         : TEXT("quirks"));
    }
    if (status == SXML_STATUS_OK && is_start) {
        const size_t self_closing = json_element(document, token, 3);
        status = write_field(writer, self_closing != SXML_TEST_NO_VALUE &&
                                             json_at(document, self_closing)->value_kind ==
                                                 SXML_TEST_JSON_KIND_TRUE
                                         ? TEXT("/")
                                         : TEXT(""));
        sxml_xml_buffer attribute_text = sxml_xml_buffer_empty(writer->output->allocator);
        attribute_list list = {.attribute_count = 0, .text = &attribute_text};
        const size_t attributes = json_element(document, token, 2);
        for (size_t member = attributes == SXML_TEST_NO_VALUE
                                 ? SXML_TEST_NO_VALUE
                                 : json_at(document, attributes)->first_child;
             status == SXML_STATUS_OK && member != SXML_TEST_NO_VALUE && list.attribute_count < 64;
             member = json_at(document, member)->next_sibling) {
            token_attribute *attribute = &list.attributes[list.attribute_count];
            const size_t start = attribute_text.byte_count;
            status = sxml_xml_buffer_append(
                &attribute_text,
                sxml_xml_buffer_view(&document->text, json_at(document, member)->key_range));
            attribute->attribute_name = (sxml_byte_range){
                .byte_offset = start, .byte_count = attribute_text.byte_count - start};
            if (status == SXML_STATUS_OK) {
                status =
                    copy_string(&(string_copy){.document = document,
                                               .value_index = member,
                                               .is_double_escaped = request->is_double_escaped},
                                &attribute_text, &attribute->attribute_value);
            }
            list.attribute_count += 1;
        }
        if (status == SXML_STATUS_OK) {
            status = write_attribute_list(writer, &list);
        }
        sxml_xml_buffer_release(&attribute_text);
    }
    return status;
}

static sxml_status write_expected_tokens(const expected_request *request, token_writer *writer) {
    sxml_status status = SXML_STATUS_OK;
    const json_document *document = request->document;
    for (size_t token = json_at(document, request->output)->first_child;
         status == SXML_STATUS_OK && token != SXML_TEST_NO_VALUE;
         token = json_at(document, token)->next_sibling) {
        status = write_expected_fields(writer, request, token);
    }
    return status;
}

/* Writes one token the tokenizer gave. */
static sxml_status write_actual_token(token_writer *writer, const sxml_html_token *token) {
    if (token->token_kind == SXML_HTML_TOKEN_KIND_CHARACTERS) {
        return token->data.byte_count == 0
                   ? SXML_STATUS_OK
                   : write_characters(
                         writer, &(test_text){.text = token->data, .is_double_escaped = false});
    }
    static const uint8_t letters[] = {'C', 'S', 'E', 'M', 'D', 'P', 'F'};
    sxml_status status = start_token(writer, letters[token->token_kind]);
    sxml_xml_buffer field = sxml_xml_buffer_empty(writer->output->allocator);
    switch (token->token_kind) {
    case SXML_HTML_TOKEN_KIND_START_TAG:
    case SXML_HTML_TOKEN_KIND_END_TAG:
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(&field, '"');
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&field, token->name);
        }
        if (status == SXML_STATUS_OK) {
            status = write_field(writer, view_of(&field));
        }
        break;
    case SXML_HTML_TOKEN_KIND_COMMENT:
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(&field, '"');
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&field, token->data);
        }
        if (status == SXML_STATUS_OK) {
            status = write_field(writer, view_of(&field));
        }
        break;
    case SXML_HTML_TOKEN_KIND_PROCESSING_INSTRUCTION:
        if (status == SXML_STATUS_OK) {
            status = write_field(writer, token->name);
        }
        if (status == SXML_STATUS_OK) {
            status = write_field(writer, token->data);
        }
        break;
    case SXML_HTML_TOKEN_KIND_DOCTYPE: {
        const bool is_present[] = {token->has_name, token->has_public_identifier,
                                   token->has_system_identifier};
        const sxml_byte_view parts[] = {token->name, token->public_identifier,
                                        token->system_identifier};
        for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < 3; part_index += 1) {
            field.byte_count = 0;
            if (is_present[part_index]) {
                status = sxml_xml_buffer_append_byte(&field, '"');
                if (status == SXML_STATUS_OK) {
                    status = sxml_xml_buffer_append(&field, parts[part_index]);
                }
            } else {
                status = sxml_xml_buffer_append(&field, TEXT("null"));
            }
            if (status == SXML_STATUS_OK) {
                status = write_field(writer, view_of(&field));
            }
        }
        if (status == SXML_STATUS_OK) {
            status = write_field(writer, token->forces_quirks ? TEXT("quirks") : TEXT("correct"));
        }
        break;
    }
    case SXML_HTML_TOKEN_KIND_CHARACTERS:
    case SXML_HTML_TOKEN_KIND_END_OF_FILE:
        break;
    }
    sxml_xml_buffer_release(&field);
    if (status == SXML_STATUS_OK && token->token_kind == SXML_HTML_TOKEN_KIND_START_TAG) {
        status = write_field(writer, token->is_self_closing ? TEXT("/") : TEXT(""));
        sxml_xml_buffer attribute_text = sxml_xml_buffer_empty(writer->output->allocator);
        attribute_list list = {.attribute_count = 0, .text = &attribute_text};
        for (size_t attribute_index = 0;
             status == SXML_STATUS_OK && attribute_index < token->attributes.attribute_count &&
             attribute_index < 64;
             attribute_index += 1) {
            const sxml_html_token_attribute *attribute =
                &token->attributes.attributes[attribute_index];
            const size_t name_start = attribute_text.byte_count;
            status = sxml_xml_buffer_append(&attribute_text, attribute->attribute_name);
            const size_t value_start = attribute_text.byte_count;
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append(&attribute_text, attribute->attribute_value);
            }
            list.attributes[attribute_index] = (token_attribute){
                .attribute_name = {.byte_offset = name_start,
                                   .byte_count = value_start - name_start},
                .attribute_value = {.byte_offset = value_start,
                                    .byte_count = attribute_text.byte_count - value_start}};
            list.attribute_count += 1;
        }
        if (status == SXML_STATUS_OK) {
            status = write_attribute_list(writer, &list);
        }
        sxml_xml_buffer_release(&attribute_text);
    }
    return status;
}

/* The initial states of the tests (13.2.5). */
typedef struct initial_state {
    sxml_byte_view state_name;
    sxml_html_content_state content_state;
    bool is_cdata_section;
} initial_state;

static const initial_state initial_states[] = {
    {SXML_BYTE_VIEW_INITIALIZER("Data state"), SXML_HTML_CONTENT_STATE_DATA, false},
    {SXML_BYTE_VIEW_INITIALIZER("PLAINTEXT state"), SXML_HTML_CONTENT_STATE_PLAINTEXT, false},
    {SXML_BYTE_VIEW_INITIALIZER("RCDATA state"), SXML_HTML_CONTENT_STATE_RCDATA, false},
    {SXML_BYTE_VIEW_INITIALIZER("RAWTEXT state"), SXML_HTML_CONTENT_STATE_RAWTEXT, false},
    {SXML_BYTE_VIEW_INITIALIZER("Script data state"), SXML_HTML_CONTENT_STATE_SCRIPT_DATA, false},
    {SXML_BYTE_VIEW_INITIALIZER("CDATA section state"), SXML_HTML_CONTENT_STATE_DATA, true},
};

/* One tokenizer run: an input in a state. */
typedef struct tokenizer_request {
    sxml_byte_view input;
    const initial_state *state;
    sxml_byte_view last_start_tag;
} tokenizer_request;

static sxml_status tokenize(const tokenizer_request *request, token_writer *writer) {
    const sxml_allocator allocator = writer->output->allocator;
    sxml_xml_buffer stream = sxml_xml_buffer_empty(allocator);
    bool has_stream_error = false;
    sxml_status status = make_stream(
        &(stream_request){.input = request->input,
                          .prefix = request->state->is_cdata_section
                                        ? TEXT("<![CDATA[")
                                        : (sxml_byte_view){.bytes = NULL, .byte_count = 0},
                          .has_parse_error = &has_stream_error},
        &stream);
    if (status != SXML_STATUS_OK) {
        sxml_xml_buffer_release(&stream);
        return status;
    }
    sxml_html_tokenizer tokenizer;
    sxml_html_tokenizer_initialize(
        &tokenizer,
        &(sxml_html_tokenizer_configuration){.allocator = allocator, .input = view_of(&stream)});
    tokenizer.allows_cdata_sections = request->state->is_cdata_section;
    if (request->state->content_state != SXML_HTML_CONTENT_STATE_DATA) {
        sxml_html_tokenizer_switch_state(&tokenizer, request->state->content_state);
    }
    status = sxml_xml_buffer_append(&tokenizer.last_start_tag_name, request->last_start_tag);
    bool is_at_end = false;
    while (status == SXML_STATUS_OK && !is_at_end) {
        sxml_html_token token;
        status = sxml_html_tokenizer_next(&tokenizer, &token);
        if (status == SXML_STATUS_OK) {
            is_at_end = token.token_kind == SXML_HTML_TOKEN_KIND_END_OF_FILE;
            if (!is_at_end) {
                status = write_actual_token(writer, &token);
            }
        }
    }
    writer->has_parse_error = has_stream_error || tokenizer.has_parse_error;
    sxml_html_tokenizer_finalize(&tokenizer);
    sxml_xml_buffer_release(&stream);
    return status;
}

/* Runs one test in each of its initial states. */
typedef struct tokenizer_run {
    sxml_test_state *state;
    suite_counts *counts;
    const json_document *document;
    const char *file_name;
    size_t test_number;
} tokenizer_run;

static void run_tokenizer_state(const tokenizer_run *run, size_t test, const initial_state *state) {
    const json_document *document = run->document;
    const size_t double_escaped =
        json_member(document, (json_member_request){.object = test, .key = TEXT("doubleEscaped")});
    const bool is_double_escaped =
        double_escaped != SXML_TEST_NO_VALUE &&
        json_at(document, double_escaped)->value_kind == SXML_TEST_JSON_KIND_TRUE;
    const size_t input_value =
        json_member(document, (json_member_request){.object = test, .key = TEXT("input")});
    const size_t output_value =
        json_member(document, (json_member_request){.object = test, .key = TEXT("output")});
    const size_t last_start_tag =
        json_member(document, (json_member_request){.object = test, .key = TEXT("lastStartTag")});
    const size_t errors =
        json_member(document, (json_member_request){.object = test, .key = TEXT("errors")});
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_xml_buffer input = sxml_xml_buffer_empty(allocator);
    sxml_xml_buffer expected = sxml_xml_buffer_empty(allocator);
    sxml_xml_buffer actual = sxml_xml_buffer_empty(allocator);
    bool has_lone_surrogate = json_at(document, input_value)->has_lone_surrogate;
    sxml_status status = append_test_text(
        &(test_text){.text = json_string(document, input_value),
                     .is_double_escaped = is_double_escaped},
        &(text_destination){.output = &input, .has_lone_surrogate = &has_lone_surrogate});
    token_writer expected_writer = {.output = &expected,
                                    .follows_characters = false,
                                    .has_lone_surrogate = false,
                                    .has_parse_error = false};
    if (status == SXML_STATUS_OK && output_value != SXML_TEST_NO_VALUE) {
        status = write_expected_tokens(&(expected_request){.document = document,
                                                           .output = output_value,
                                                           .is_double_escaped = is_double_escaped},
                                       &expected_writer);
    }
    if (has_lone_surrogate || expected_writer.has_lone_surrogate) {
        run->counts->left_out_count += 1;
    } else {
        token_writer actual_writer = {.output = &actual,
                                      .follows_characters = false,
                                      .has_lone_surrogate = false,
                                      .has_parse_error = false};
        if (status == SXML_STATUS_OK) {
            status = tokenize(
                &(tokenizer_request){.input = view_of(&input),
                                     .state = state,
                                     .last_start_tag =
                                         last_start_tag == SXML_TEST_NO_VALUE
                                             ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                             : json_string(document, last_start_tag)},
                &actual_writer);
        }
        SXML_TEST_EXPECT(run->state, status == SXML_STATUS_OK);
        char case_name[SXML_TEST_NAME_BYTE_COUNT];
        (void)snprintf(case_name, sizeof(case_name), "%s:%zu:%.*s", run->file_name,
                       run->test_number, (int)state->state_name.byte_count,
                       (const char *)state->state_name.bytes);
        count_outcome(run->counts,
                      &(case_outcome){
                          .case_name = case_name,
                          .has_passed = status == SXML_STATUS_OK &&
                                        sxml_byte_view_equals(view_of(&expected), view_of(&actual)),
                          .expected = view_of(&expected),
                          .actual = view_of(&actual)});
        const bool expects_error =
            errors != SXML_TEST_NO_VALUE && json_at(document, errors)->child_count > 0;
        const bool has_parse_error = actual_writer.has_parse_error;
        if (has_parse_error == expects_error) {
            run->counts->error_agreement_count += 1;
        } else {
            run->counts->error_disagreement_count += 1;
            run->counts->missed_error_count += has_parse_error ? 0 : 1;
        }
    }
    sxml_xml_buffer_release(&actual);
    sxml_xml_buffer_release(&expected);
    sxml_xml_buffer_release(&input);
}

static void run_tokenizer_test(const tokenizer_run *run, size_t test) {
    const json_document *document = run->document;
    const size_t states =
        json_member(document, (json_member_request){.object = test, .key = TEXT("initialStates")});
    if (states == SXML_TEST_NO_VALUE) {
        run_tokenizer_state(run, test, &initial_states[0]);
        return;
    }
    for (size_t state_value = json_at(document, states)->first_child;
         state_value != SXML_TEST_NO_VALUE;
         state_value = json_at(document, state_value)->next_sibling) {
        const initial_state *state = NULL;
        for (size_t state_index = 0; state_index < SXML_TEST_COUNT(initial_states);
             state_index += 1) {
            if (sxml_byte_view_equals(json_string(document, state_value),
                                      initial_states[state_index].state_name)) {
                state = &initial_states[state_index];
            }
        }
        SXML_TEST_EXPECT(run->state, state != NULL);
        if (state != NULL) {
            run_tokenizer_state(run, test, state);
        }
    }
}

static void run_tokenizer_file(sxml_test_state *state, const char *file_name,
                               suite_counts *counts) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    suite_directory(directory);
    char path[SXML_TEST_PATH_BYTE_COUNT + 64];
    (void)snprintf(path, sizeof(path), "%stokenizer/%s", directory, file_name);
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_xml_buffer contents = sxml_xml_buffer_empty(allocator);
    SXML_TEST_EXPECT(state, read_file(path, &contents));
    json_document document = {
        .values = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(json_value)}),
        .text = sxml_xml_buffer_empty(allocator)};
    SXML_TEST_EXPECT(state, read_json(view_of(&contents), &document) == SXML_STATUS_OK);
    const size_t tests =
        document.values.element_count == 0
            ? SXML_TEST_NO_VALUE
            : json_member(&document, (json_member_request){.object = 0, .key = TEXT("tests")});
    SXML_TEST_EXPECT(state, tests != SXML_TEST_NO_VALUE);
    size_t test_number = 1;
    for (size_t test_value = tests == SXML_TEST_NO_VALUE ? SXML_TEST_NO_VALUE
                                                         : json_at(&document, tests)->first_child;
         test_value != SXML_TEST_NO_VALUE;
         test_value = json_at(&document, test_value)->next_sibling) {
        run_tokenizer_test(&(tokenizer_run){.state = state,
                                            .counts = counts,
                                            .document = &document,
                                            .file_name = file_name,
                                            .test_number = test_number},
                           test_value);
        test_number += 1;
    }
    sxml_xml_array_release(&document.values);
    sxml_xml_buffer_release(&document.text);
    sxml_xml_buffer_release(&contents);
}

static const char *const tokenizer_file_names[] = {
    "contentModelFlags.test",
    "domjs.test",
    "entities.test",
    "escapeFlag.test",
    "namedEntities.test",
    "numericEntities.test",
    "pendingSpecChanges.test",
    "test1.test",
    "test2.test",
    "test3.test",
    "test4.test",
    "unicodeChars.test",
    "unicodeCharsProblematic.test",
};

static void test_tokenizer(sxml_test_state *state) {
    if (!suite_is_reachable("tokenizer/test1.test")) {
        return;
    }
    suite_counts counts;
    memset(&counts, 0, sizeof(counts));
    for (size_t file_index = 0; file_index < SXML_TEST_COUNT(tokenizer_file_names);
         file_index += 1) {
        run_tokenizer_file(state, tokenizer_file_names[file_index], &counts);
    }
    report_counts("tokenizer", &counts);
    SXML_TEST_EXPECT(state, counts.unexpected_count == 0);
    SXML_TEST_EXPECT(state, counts.error_disagreement_count == 0);
    SXML_TEST_EXPECT(state, counts.fixed_count == 0);
    SXML_TEST_EXPECT(state, counts.passed_count > 7000);
}

static const sxml_test_case html5lib_test_cases[] = {
    {.name = "tree_construction", .function = test_tree_construction},
    {.name = "tokenizer", .function = test_tokenizer},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "html5lib",
            .cases = html5lib_test_cases,
            .case_count = SXML_TEST_COUNT(html5lib_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
