/*
 * Tests for CSV texts and the CSV part of the function library
 * (src/csv.c, src/function_library_csv.c; D97, D141): the scanner's
 * tokens and decoding, then fn:parse-csv, fn:csv-to-xml and fn:csv-doc
 * through the public API. Values are written by the adaptive method,
 * errors checked by code. Each case checks that no cell or allocation
 * leaked, and one runs a mix of the functions under every allocation
 * failure.
 *
 * Golden file: tests/function_library_uri_test.c (fixture, running, case
 * tables). Cases follow the F&O 4.0 examples of section 17.5 and the QT4
 * test sets fn-parse-csv, fn-csv-to-xml and fn-csv-doc.
 */
#include "csv_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Scanner */

static const sxml_csv_dialect comma_dialect = {
    .separator = SXML_BYTE_VIEW_INITIALIZER(","),
    .quote_character = SXML_BYTE_VIEW_INITIALIZER("\""),
    .comment_marker = {.bytes = NULL, .byte_count = 0},
    .trims_whitespace = false,
};

/*
 * Writes the tokens of csv_text into description: each field as its
 * value in brackets ("q" before a quoted one), "/" for a row end, "." for
 * the input end, "!" for a syntax error. Returns false when description
 * is too short.
 */
typedef struct token_listing {
    char description[256];
} token_listing;

typedef struct scan_case {
    sxml_csv_dialect dialect;
    const char *csv_text;
} scan_case;

static bool append_text(token_listing *listing, size_t *length, sxml_byte_view text) {
    if (*length + text.byte_count + 1 > sizeof(listing->description)) {
        return false;
    }
    if (text.byte_count > 0) {
        memcpy(listing->description + *length, text.bytes, text.byte_count);
    }
    *length += text.byte_count;
    listing->description[*length] = '\0';
    return true;
}

static bool list_tokens(const scan_case *test_case, token_listing *listing) {
    const sxml_csv_source source = {.csv_text = sxml_byte_view_from_c_string(test_case->csv_text),
                                    .dialect = test_case->dialect};
    sxml_csv_cursor cursor = sxml_csv_cursor_start();
    uint8_t decoded[128];
    size_t length = 0;
    listing->description[0] = '\0';
    for (size_t token_number = 0; token_number < 64; token_number += 1) {
        sxml_csv_token token;
        if (sxml_csv_read_token(&source, &cursor, &token) != SXML_STATUS_OK) {
            return append_text(listing, &length, TEXT("!"));
        }
        if (token.token_kind == SXML_CSV_TOKEN_KIND_INPUT_END) {
            return append_text(listing, &length, TEXT("."));
        }
        if (token.token_kind == SXML_CSV_TOKEN_KIND_ROW_END) {
            if (!append_text(listing, &length, TEXT("/"))) {
                return false;
            }
            continue;
        }
        sxml_byte_view value = token.field_text;
        if (token.needs_decoding &&
            sxml_csv_decode_field(&(sxml_csv_field_decoding){.token = &token,
                                                             .quote_character =
                                                                 test_case->dialect.quote_character},
                                  (sxml_memory_view){.bytes = decoded, .byte_count = sizeof(decoded)},
                                  &value) != SXML_STATUS_OK) {
            return false;
        }
        if (!append_text(listing, &length, token.is_quoted ? TEXT("q[") : TEXT("[")) ||
            !append_text(listing, &length, value) || !append_text(listing, &length, TEXT("]"))) {
            return false;
        }
    }
    return false;
}

static bool scans_to(scan_case test_case, const char *expected) {
    token_listing listing;
    const bool holds = list_tokens(&test_case, &listing) && strcmp(listing.description, expected) == 0;
    if (!holds) {
        (void)fprintf(stderr, "scan of %s: %s, expected %s\n", test_case.csv_text,
                      listing.description, expected);
    }
    return holds;
}

static scan_case comma_case(const char *csv_text) {
    return (scan_case){.dialect = comma_dialect, .csv_text = csv_text};
}

static void test_scanner_tokens(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, scans_to(comma_case(""), "."));
    SXML_TEST_EXPECT(state, scans_to(comma_case("\n"), "/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case("a"), "[a]/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case("a,b\r\nc,d\n"), "[a][b]/[c][d]/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case("a\r\rb\r"), "[a]//[b]/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case(",\n,,"), "[][]/[][][]/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case("\"x,\"\"y\"\"\r\nz\",w"),
                                     "q[x,\"y\"\nz][w]/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case("\"\",a\"b\""), "q[][a\"b\"]/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case(" \"a\""), "[ \"a\"]/."));
    SXML_TEST_EXPECT(state, scans_to(comma_case("\"a"), "!"));
    SXML_TEST_EXPECT(state, scans_to(comma_case("\"a\"b"), "!"));
    SXML_TEST_EXPECT(state, scans_to(comma_case("\"a\" ,b"), "!"));
}

static void test_scanner_dialects(sxml_test_state *state) {
    sxml_csv_dialect trimming = comma_dialect;
    trimming.trims_whitespace = true;
    SXML_TEST_EXPECT(state, scans_to((scan_case){.dialect = trimming, .csv_text = " a ,\" b\", \" c\"\n  \n "},
                                     "[a]q[ b][\" c\"]//."));
    SXML_TEST_EXPECT(state, scans_to((scan_case){.dialect = trimming, .csv_text = " \"a\""},
                                     "[\"a\"]/."));
    const sxml_csv_dialect commented = {.separator = TEXT(";"),
                                        .quote_character = TEXT("'"),
                                        .comment_marker = TEXT("#"),
                                        .trims_whitespace = false};
    SXML_TEST_EXPECT(state, scans_to((scan_case){.dialect = commented,
                                                 .csv_text = "#x\n#y\r\na;'b;''c'\n#z"},
                                     "[a]q[b;'c]/."));
    /* Multi-byte separator and quote. */
    const sxml_csv_dialect wide = {.separator = TEXT("\xC2\xA7"),
                                   .quote_character = TEXT("\xE2\x80\x9C"),
                                   .comment_marker = {.bytes = NULL, .byte_count = 0},
                                   .trims_whitespace = false};
    SXML_TEST_EXPECT(state, scans_to((scan_case){.dialect = wide,
                                                 .csv_text = "a\xC2\xA7\xE2\x80\x9C" "b\xC2\xA7\xE2\x80\x9C"},
                                     "[a]q[b\xC2\xA7]/."));
    SXML_TEST_EXPECT(state, sxml_csv_dialect_is_valid(&comma_dialect));
    SXML_TEST_EXPECT(state, sxml_csv_dialect_is_valid(&commented));
    SXML_TEST_EXPECT(state, sxml_csv_dialect_is_valid(&wide));
    sxml_csv_dialect invalid = comma_dialect;
    invalid.separator = TEXT("::");
    SXML_TEST_EXPECT(state, !sxml_csv_dialect_is_valid(&invalid));
    invalid.separator = TEXT("\n");
    SXML_TEST_EXPECT(state, !sxml_csv_dialect_is_valid(&invalid));
    invalid.separator = TEXT("\"");
    SXML_TEST_EXPECT(state, !sxml_csv_dialect_is_valid(&invalid));
    invalid = commented;
    invalid.comment_marker = TEXT(";");
    SXML_TEST_EXPECT(state, !sxml_csv_dialect_is_valid(&invalid));
    invalid.comment_marker = TEXT("\xC2");
    SXML_TEST_EXPECT(state, !sxml_csv_dialect_is_valid(&invalid));
    uint8_t too_short[1];
    const sxml_csv_token token = {.token_kind = SXML_CSV_TOKEN_KIND_FIELD,
                                  .field_text = TEXT("a\"\""),
                                  .is_quoted = true,
                                  .needs_decoding = true};
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state, sxml_csv_decode_field(
                                &(sxml_csv_field_decoding){.token = &token, .quote_character = TEXT("\"")},
                                (sxml_memory_view){.bytes = too_short, .byte_count = 1},
                                &value) == SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_csv_trim_whitespace(TEXT(" \t a b\r\n")),
                                                  TEXT("a b")));
}

/* ---------------------------------------------------------------- Fixture */

/* 2026-10-16T12:30:45.5-05:00, implicit timezone +02:00. */
static const sxml_clock_reading fixture_clock_reading = {
    .current_date_time = {.year = 2026,
                          .month = 10,
                          .day = 16,
                          .hour = 12,
                          .minute = 30,
                          .second = 45,
                          .nanosecond = 500000000,
                          .has_timezone = true,
                          .timezone_offset_minutes = -300},
    .implicit_timezone_offset_minutes = 120,
};

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
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
        &(sxml_processor_configuration){.allocator = allocator,
                                        .clock = sxml_fixed_clock(&fixture_clock_reading)},
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

/* Destroys the fixture; true when no cell and no allocation leaked. */
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

/* --------------------------------------------------------------- Running */

/* Compiles and evaluates expression_text and writes its value by the
   adaptive method, items separated by a space, into the fixture's output
   buffer (cleared first). error_record receives the failure. */
typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_error_record error_record;
} query_run;

static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){.processor = test_fixture->processor,
                                               .expression_text = run->expression_text,
                                               .source_name = TEXT("test.xpath"),
                                               .error_record = &run->error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xpath_expression_evaluate(
        expression, &(sxml_xpath_evaluation_request){.error_record = &run->error_record},
        &result);
    sxml_xpath_expression_destroy(expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_serialize_sequence(
        sxml_processor_get_item_heap(test_fixture->processor),
        &(sxml_serialization_request){
            .value = result,
            .serialization_parameters = {.serialization_method =
                                             SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                         .has_item_separator = true,
                                         .item_separator = TEXT(" ")},
            .error_record = &run->error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    sxml_sequence_release(result);
    return status;
}

/* An expression and its adaptive output, or the code of the error it
   raises. */
typedef struct expression_case {
    const char *expression_text;
    const char *expected_output;
    const char *expected_error_code;
} expression_case;

#define VALUE_CASE(text, output)                                              \
    {.expression_text = (text), .expected_output = (output), .expected_error_code = NULL}
#define ERROR_CASE(text, code)                                                \
    {.expression_text = (text), .expected_output = NULL, .expected_error_code = (code)}

static bool case_holds(const fixture *test_fixture, const expression_case *test_case) {
    query_run run = {.expression_text = sxml_byte_view_from_c_string(test_case->expression_text)};
    const sxml_status status = run_query(test_fixture, &run);
    bool holds = false;
    if (test_case->expected_error_code != NULL) {
        holds = status != SXML_STATUS_OK &&
                sxml_byte_view_equals(run.error_record.code.local_name,
                                      sxml_byte_view_from_c_string(test_case->expected_error_code));
    } else {
        holds = status == SXML_STATUS_OK &&
                sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                      sxml_byte_view_from_c_string(test_case->expected_output));
    }
    if (!holds) {
        const sxml_byte_view output = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
        const sxml_byte_view status_name = sxml_status_name(status);
        (void)fprintf(stderr, "case %s: status %.*s, error %.*s, output %.*s\n",
                      test_case->expression_text, (int)status_name.byte_count,
                      (const char *)status_name.bytes,
                      (int)run.error_record.code.local_name.byte_count,
                      (const char *)run.error_record.code.local_name.bytes,
                      (int)output.byte_count, (const char *)output.bytes);
    }
    return holds;
}

/* Runs every case in one processor, then checks for leaks. */
static void expect_all(sxml_test_state *state, const expression_case *cases,
                       size_t case_count) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        SXML_TEST_EXPECT(state, case_holds(&test_fixture, &cases[case_index]));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------ parse-csv */

static void test_parse_csv(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("parse-csv(())", ""),
        VALUE_CASE("parse-csv('') ! (count(?rows), count(?columns), map:size(?column-index))",
                   "0 0 0"),
        VALUE_CASE("parse-csv(char(10))?rows", "[]"),
        VALUE_CASE("parse-csv(' ', {'trim-whitespace': true()})?rows", ""),
        VALUE_CASE("parse-csv(' ')?rows", "[\" \"]"),
        VALUE_CASE("parse-csv(string-join(('name,city', 'Bob,Berlin', 'Alice,Aachen'), char(10))) ! "
                   "(?rows, ?get(2, 2))",
                   "[\"name\",\"city\"] [\"Bob\",\"Berlin\"] [\"Alice\",\"Aachen\"] \"Berlin\""),
        VALUE_CASE("parse-csv('name,city' || char(10) || 'Bob,Berlin', {'header': true()}) ! "
                   "(?columns, ?column-index?city, ?get(1, 'name'), ?get(9, 1), ?get(1, 9))",
                   "\"name\" \"city\" 2 \"Bob\" \"\" \"\""),
        VALUE_CASE("parse-csv('|name|;|city|' || char(10) || '|Bob|;|Berlin|', "
                   "{'separator': ';', 'quote-character': '|'})?rows[2]",
                   "[\"Bob\",\"Berlin\"]"),
        VALUE_CASE("parse-csv('a,b,c,d' || char(10) || '1,2', {'header': ('P', '', 'A'), "
                   "'select-columns': (2, 1, 4)}) ! (?rows, map:keys(?column-index), ?get(2, 'A'))",
                   "[\"b\",\"a\",\"d\"] [\"2\",\"1\",\"\"] \"P\" \"A\" \"\""),
        VALUE_CASE("parse-csv(' h1 ,h1,\" h3 \",' || char(10) || '1,2,3', "
                   "{'header': true(), 'select-columns': (3, 1, 9)}) ! (?columns, "
                   "?column-index?h1, ?rows)",
                   "\"h3\" \"h1\" \"\" 2 [\"3\",\"1\",\"\"]"),
        VALUE_CASE("parse-csv('a,b,c' || char(10) || '1' || char(10) || '1,2,3,4', "
                   "{'trim-rows': true()})?rows",
                   "[\"a\",\"b\",\"c\"] [\"1\",\"\",\"\"] [\"1\",\"2\",\"3\"]"),
        VALUE_CASE("parse-csv('#c' || char(13) || char(10) || 'a,b', {'comment-marker': '#'})?rows",
                   "[\"a\",\"b\"]"),
        VALUE_CASE("parse-csv('Monitor,24\" wide,Robert \"Bob\" Smith')?rows?2", "\"24\"\" wide\""),
        VALUE_CASE("parse-csv('a', {'select-columns': (4294967298, 1)})?rows", "[\"\",\"a\"]"),
        VALUE_CASE("parse-csv('a') instance of fn:parsed-csv-structure-record", "true()"),
        VALUE_CASE("parse-csv('a')?get instance of fn(xs:positiveInteger, xs:string) as xs:string",
                   "true()"),
        ERROR_CASE("parse-csv('\"Bob\" Smith,Berlin')", "FOCV0001"),
        ERROR_CASE("parse-csv('one,two,\"')", "FOCV0001"),
        ERROR_CASE("parse-csv('a', {'separator': '::'})", "FOCV0002"),
        ERROR_CASE("parse-csv('a', {'separator': char(10)})", "FOCV0002"),
        ERROR_CASE("parse-csv('a', {'quote-character': ','})", "FOCV0002"),
        ERROR_CASE("parse-csv('a', {'comment-marker': ''})", "FOCV0002"),
        ERROR_CASE("parse-csv('a', {'row-delimiter': ';'})", "XPTY0004"),
        ERROR_CASE("parse-csv('a', {'header': (true(), 'x')})", "XPTY0004"),
        ERROR_CASE("parse-csv('a,b', {'header': true()})?get(1, 'c')", "FOCV0004"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ----------------------------------------------------------- csv-to-xml */

static void test_csv_to_xml(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("csv-to-xml(())", ""),
        VALUE_CASE("csv-to-xml('')",
                   "<csv xmlns=\"http://www.w3.org/2005/xpath-functions\"><rows/></csv>"),
        VALUE_CASE("csv-to-xml(char(10))",
                   "<csv xmlns=\"http://www.w3.org/2005/xpath-functions\"><rows><row/></rows></csv>"),
        VALUE_CASE("csv-to-xml('', {'header': ('name', '', 'city', '')})",
                   "<csv xmlns=\"http://www.w3.org/2005/xpath-functions\"><columns><column>name"
                   "</column><column/><column>city</column></columns><rows/></csv>"),
        VALUE_CASE("csv-to-xml(',b' || char(10) || '1,2,3', {'header': true()})",
                   "<csv xmlns=\"http://www.w3.org/2005/xpath-functions\"><columns><column/>"
                   "<column>b</column></columns><rows><row><field>1</field><field column=\"b\">2"
                   "</field><field>3</field></row></rows></csv>"),
        VALUE_CASE("csv-to-xml('a') instance of document-node(element(fn:csv))", "true()"),
        ERROR_CASE("csv-to-xml('\"a\"b')", "FOCV0001"),
        ERROR_CASE("csv-to-xml('a', {'separator': '\"'})", "FOCV0002"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* -------------------------------------------------------------- csv-doc */

static void test_csv_doc(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("csv-doc(())", ""),
        /* No resolver: nothing can be retrieved. */
        ERROR_CASE("csv-doc('file:///none.csv')", "FOUT1170"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------- Allocation failures */

static const char failure_expression[] =
    "(parse-csv('h,\"i\"\"j\"' || char(13) || char(10) || '1,2,3' || char(10) || char(10) || "
    "'4', {'header': true(), 'trim-rows': true()}) ! (?rows, ?get(1, 'i\"j')), "
    "csv-to-xml('x,y' || char(10) || '1', {'header': true(), 'select-columns': (2, 1)}))";

static const char failure_output[] =
    "[\"1\",\"2\"] [\"\",\"\"] [\"4\",\"\"] \"2\" <csv xmlns=\"http://www.w3.org/2005/xpath-functions\">"
    "<columns><column>y</column><column>x</column></columns><rows><row><field column=\"y\"/>"
    "<field column=\"x\">1</field></row></rows></csv>";

/* Runs the mix with allocation number failing_number failing; returns the
   status and checks the output and that nothing leaked. */
static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {.expression_text = sxml_byte_view_from_c_string(failure_expression)};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            const bool holds = sxml_byte_view_equals(
                sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                sxml_byte_view_from_c_string(failure_output));
            if (!holds) {
                const sxml_byte_view output = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
                (void)fprintf(stderr, "mix: %.*s\n", (int)output.byte_count,
                              (const char *)output.bytes);
            }
            SXML_TEST_EXPECT(state, holds);
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_functions_survive_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        status = run_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case csv_test_cases[] = {
    {.name = "scanner_tokens", .function = test_scanner_tokens},
    {.name = "scanner_dialects", .function = test_scanner_dialects},
    {.name = "parse_csv", .function = test_parse_csv},
    {.name = "csv_to_xml", .function = test_csv_to_xml},
    {.name = "csv_doc", .function = test_csv_doc},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "csv",
            .cases = csv_test_cases,
            .case_count = SXML_TEST_COUNT(csv_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
