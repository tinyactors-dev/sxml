/*
 * Tests for the EXPath Binary feature (src/function_library_binary.c,
 * packet 7.8, D198) through the public API: the bin: functions of EXPath
 * Binary 4.0 evaluated by a processor that names sxml_expath_binary_
 * feature(), their errors in the bin namespace, their absence without the
 * feature (XPST0017), and an allocation-failure loop (D23). Every case
 * checks for leaked cells and allocations.
 *
 * Golden file: tests/xpath_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The functions' namespace as a URIQualifiedName prefix. */
#define BIN "Q{http://expath.org/ns/binary}"

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

typedef struct fixture_options {
    uint64_t failing_allocation_number;
    bool names_feature;
} fixture_options;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(const fixture_options *options, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     options->failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    const sxml_optional_feature feature = sxml_expath_binary_feature();
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = options->names_feature ? 1 : 0}},
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

/* Compiles and evaluates expression_text and writes the result by the
   adaptive method into the fixture's output buffer (cleared first). */
static sxml_status run_query(const fixture *test_fixture, sxml_byte_view expression_text,
                             sxml_error_record *error_record) {
    memset(error_record, 0, sizeof(*error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){.processor = test_fixture->processor,
                                               .expression_text = expression_text,
                                               .source_name = TEXT("test.xpath"),
                                               .error_record = error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xpath_expression_evaluate(
        expression, &(sxml_xpath_evaluation_request){.error_record = error_record}, &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = true,
                                             .item_separator = TEXT(" ")},
                .error_record = error_record},
            sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    }
    sxml_sequence_release(result);
    sxml_xpath_expression_destroy(expression);
    return status;
}

static sxml_byte_view text_of(const char *text) {
    return (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = strlen(text)};
}

/* Runs pair[0] and checks its adaptive output pair[1], or the error code
   it raises ("!CODE", the local name; "!bin:CODE" also checks the bin
   namespace). */
static void expect_query(sxml_test_state *state, const fixture *test_fixture,
                         const char *const pair[2]) {
    sxml_error_record error_record;
    const sxml_status status = run_query(test_fixture, text_of(pair[0]), &error_record);
    const char *expected = pair[1];
    if (expected[0] != '!') {
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                    text_of(expected)));
        return;
    }
    SXML_TEST_EXPECT(state, sxml_status_has_error_record(status));
    const char *code = expected + 1;
    if (strncmp(code, "bin:", 4) == 0) {
        code += 4;
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.namespace_uri,
                                                      TEXT("http://expath.org/ns/binary")));
    }
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name, text_of(code)));
}

/* Runs every case on one processor with the feature. */
static void expect_queries(sxml_test_state *state, const char *const (*cases)[2],
                           size_t case_count) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.names_feature = true},
                                            &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        expect_query(state, &test_fixture, cases[case_index]);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------------ Cases */

/* Constants and conversions (EXPath Binary 4.0, section 4). */
static void test_constants_and_conversions(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {"string(" BIN "hex('1122_3F4E'))", "\"ESI/Tg==\""},
        {"xs:hexBinary(" BIN "hex('122 3F4E'))", "xs:hexBinary(\"01223F4E\")"},
        {"string(xs:hexBinary(" BIN "hex('FFF')))", "\"0FFF\""},
        {BIN "hex(())", ""},
        {BIN "hex('4X')", "!bin:non-numeric-character"},
        {"string(xs:hexBinary(" BIN "bin('111111111')))", "\"01FF\""},
        {"string(xs:hexBinary(" BIN "bin(' 101 ')))", "\"05\""},
        {"string(xs:hexBinary(" BIN "bin('')))", "\"\""},
        {BIN "bin('012')", "!bin:non-numeric-character"},
        {"(" BIN "octal('0'), " BIN "octal('155'), " BIN "octal('355'), " BIN
         "octal('555'), " BIN "octal('0155'), " BIN "octal('177 177 177 177')) ! "
         "string(xs:hexBinary(.))",
         "\"00\" \"6D\" \"ED\" \"016D\" \"006D\" \"03F9FCFE7F\""},
        {BIN "octal('8')", "!bin:non-numeric-character"},
        {BIN "to-octets(" BIN "hex('1122AAFF'))", "17 34 170 255"},
        {BIN "to-octets(" BIN "hex('00')) instance of xs:unsignedByte", "true()"},
        {"string(" BIN "from-octets((77, 97, 110)))", "\"TWFu\""},
        {BIN "from-octets(256)", "!XPTY0004"},
        {BIN "to-base64url(xs:hexBinary('FBFF')), " BIN "to-base64url(xs:hexBinary('FB')), " BIN
         "to-base64url(xs:hexBinary(''))",
         "\"-_8\" \"-w\" \"\""},
        {"string(xs:hexBinary(" BIN "from-base64url(' -_ 8 = ')))", "\"FBFF\""},
        {"string(xs:hexBinary(" BIN "from-base64url('-_-_')))", "\"FBFFBF\""},
        {BIN "from-base64url('+/8=')", "!bin:conversion-error"},
        {BIN "from-base64url('-_9')", "!bin:conversion-error"},
        {BIN "from-base64url('A')", "!bin:conversion-error"},
        {BIN "from-base64url('-=_8')", "!bin:conversion-error"},
        {"string(" BIN "from-base64url('===='))", "\"\""},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Basic operations (section 5). */
static void test_basic_operations(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {BIN "length(xs:hexBinary('FACE1234'))", "4"},
        {"string(xs:hexBinary(" BIN "part(xs:hexBinary('11223344556677'), 4)))", "\"556677\""},
        {"string(xs:hexBinary(" BIN "part(xs:hexBinary('11223344556677'), 5, 0)))", "\"\""},
        {BIN "part(xs:hexBinary('FF'), 2)", "!bin:index-out-of-range"},
        {BIN "part(xs:hexBinary('FF'), 0, 2)", "!bin:index-out-of-range"},
        {BIN "part(xs:hexBinary('FF'), 0, -1)", "!bin:negative-size"},
        {BIN "part(xs:hexBinary('FF'), 99999999999999999999)", "!bin:index-out-of-range"},
        {"string(xs:hexBinary(" BIN "join((xs:hexBinary('0000'), " BIN "hex('FFFF')))))",
         "\"0000FFFF\""},
        {"string(" BIN "join(()))", "\"\""},
        {"string(xs:hexBinary(" BIN "insert-before(xs:hexBinary('FFFF'), 1, xs:hexBinary('00'))))",
         "\"FF00FF\""},
        {BIN "insert-before(xs:hexBinary(''), 1, ())", "!bin:index-out-of-range"},
        {"string(xs:hexBinary(" BIN "pad-left(xs:hexBinary('0000'), 3, 255)))", "\"FFFFFF0000\""},
        {"string(xs:hexBinary(" BIN "pad-right(xs:hexBinary('01'), 2, ())))", "\"010000\""},
        {BIN "pad-right(xs:hexBinary('01'), -2)", "!bin:negative-size"},
        {BIN "find(xs:hexBinary('AABBCCDDBBCC'), 2, xs:hexBinary('BBCC'))", "4"},
        {BIN "find(xs:hexBinary('AABBCCDD'), 2, xs:hexBinary(''))", "2"},
        {BIN "find(xs:hexBinary('AABBCCDD'), 0, xs:hexBinary('FF'))", ""},
        {BIN "find(xs:hexBinary('AABB'), 3, xs:hexBinary('FF'))", "!bin:index-out-of-range"},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Text decoding and encoding (section 6). */
static void test_text_decoding_and_encoding(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {BIN "infer-encoding(xs:hexBinary('EFBBBF414243'))", "{\"encoding\":\"UTF-8\",\"offset\":3}"},
        {BIN "infer-encoding(xs:hexBinary('FFFE41004200'), 'uTf-16')",
         "{\"encoding\":\"UTF-16LE\",\"offset\":2}"},
        {BIN "infer-encoding(xs:hexBinary('0041'), 'UTF-16')",
         "{\"encoding\":\"UTF-16BE\",\"offset\":0}"},
        {BIN "infer-encoding(xs:hexBinary('41'), 'iso-8859-1')",
         "{\"encoding\":\"iso-8859-1\",\"offset\":0}"},
        {BIN "infer-encoding(xs:hexBinary(''), 'NOTutf-8')", "!bin:unknown-encoding"},
        {BIN "decode-string(xs:hexBinary('FFFE410042004300'))", "\"ABC\""},
        {BIN "decode-string(xs:hexBinary('EFBBBF41424344'), (), 3)", "\"ABCD\""},
        {BIN "decode-string(xs:hexBinary('41424344'), 'UTF-8', 1, 2)", "\"BC\""},
        {BIN "decode-string(xs:hexBinary('40EFBBBF4142'), (), 2)", "!bin:conversion-error"},
        {BIN "decode-string(xs:hexBinary('0041'), 'utf-16', 1)", "!bin:conversion-error"},
        {BIN "decode-string(xs:hexBinary('00'))", "!bin:conversion-error"},
        {BIN "decode-string(xs:hexBinary('41'), 'X')", "!bin:unknown-encoding"},
        {BIN "decode-string(xs:hexBinary('41'), (), -1)", "!bin:index-out-of-range"},
        {BIN "decode-string(xs:hexBinary('41'), (), 0, -1)", "!bin:negative-size"},
        {"string(xs:hexBinary(" BIN "encode-string('A\xC4\x9E', 'UTF-16')))", "\"0041011E\""},
        {"string(xs:hexBinary(" BIN "encode-string(char(0xFEFF) || 'A', 'utf-16le')))",
         "\"FFFE4100\""},
        {"string(xs:hexBinary(" BIN "encode-string('\xF0\x9F\x98\x80', 'UTF-16BE')))",
         "\"D83DDE00\""},
        {"string(" BIN "encode-string('A\xC3\xAA', ()))", "\"QcOq\""},
        {BIN "encode-string('\xC3\xA4', 'US-ASCII')", "!bin:conversion-error"},
        {"string(xs:hexBinary(" BIN "encode-string('\xC3\xA4', 'ISO-8859-1')))", "\"E4\""},
        {BIN "encode-string('', 'X')", "!bin:unknown-encoding"},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Packing and unpacking (section 7). */
static void test_packing_and_unpacking(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {"string(xs:hexBinary(" BIN "pack-double(1)))", "\"3FF0000000000000\""},
        {"string(xs:hexBinary(" BIN "pack-double(-0e0, 'LE')))", "\"0000000000000080\""},
        {"string(xs:hexBinary(" BIN "pack-double(xs:double('NaN'))))", "\"7FF8000000000000\""},
        {"string(xs:hexBinary(" BIN "pack-float(1, 'little-endian')))", "\"0000803F\""},
        {"string(xs:hexBinary(" BIN "pack-float(xs:float('NaN'))))", "\"7FC00000\""},
        {BIN "pack-double(1, 'X')", "!XPTY0004"},
        {"string(xs:hexBinary(" BIN "pack-integer(256, 2, 'LE')))", "\"0001\""},
        {"string(xs:hexBinary(" BIN "pack-integer(-2, 4)))", "\"FFFFFFFE\""},
        {"string(xs:hexBinary(" BIN "pack-integer(-1, 9)))", "\"FFFFFFFFFFFFFFFFFF\""},
        {"string(xs:hexBinary(" BIN "pack-integer(65536, 2)))", "\"0000\""},
        {"string(xs:hexBinary(" BIN "pack-integer(-1000000000000, 16, 'BE')))",
         "\"FFFFFFFFFFFFFFFFFFFFFF172B5AF000\""},
        {"string(xs:hexBinary(" BIN "pack-integer(18446744073709551617, 9)))",
         "\"010000000000000001\""},
        {"string(xs:hexBinary(" BIN "pack-integer(-18446744073709551617, 9)))",
         "\"FEFFFFFFFFFFFFFFFF\""},
        {BIN "pack-integer(1, -1)", "!bin:negative-size"},
        {BIN "unpack-double(xs:hexBinary('000000000000F03F'), 0, 'LE')", "1.0e0"},
        {BIN "unpack-double(xs:hexBinary('7FF0000000000001'), 0) => string()", "\"NaN\""},
        {BIN "unpack-double(xs:hexBinary('00'), 0)", "!bin:index-out-of-range"},
        {BIN "unpack-float(xs:hexBinary('3F800000'), 0) instance of xs:float", "true()"},
        {BIN "unpack-integer(xs:hexBinary('00FFFFFFFF'), 1, 4)", "-1"},
        {BIN "unpack-unsigned-integer(xs:hexBinary('00FFFFFFFF'), 1, 4)", "4294967295"},
        {BIN "unpack-integer(xs:hexBinary('FEFF'), 0, 2, 'LE')", "-2"},
        {BIN "unpack-unsigned-integer(xs:hexBinary('FFFFFFFFFFFFFFFF'), 0, 8)",
         "18446744073709551615"},
        {BIN "unpack-integer(xs:hexBinary('8000000000000000'), 0, 8)", "-9223372036854775808"},
        {BIN "unpack-integer(xs:hexBinary('FF7FFFFFFFFFFFFFFF'), 0, 9)", "-9223372036854775809"},
        {BIN "unpack-unsigned-integer(xs:hexBinary('0FFFFFFFFFFFFFFFFFFF'), 0, 10)",
         "75557863725914323419135"},
        {BIN "unpack-integer(xs:hexBinary('01'), 0, 0)", "0"},
        {BIN "unpack-integer(xs:hexBinary('00'), 0, -1)", "!bin:negative-size"},
        {BIN "unpack-integer(xs:hexBinary('00'), 0, 2)", "!bin:index-out-of-range"},
        {"let $big := 123456789123456789123456789123456789 return " BIN
         "unpack-integer(" BIN "pack-integer($big, 20), 0, 20) eq $big",
         "true()"},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Bitwise operations (section 8). */
static void test_bitwise_operations(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {"string(xs:hexBinary(" BIN "and(xs:hexBinary('8081'), xs:hexBinary('7F7E'))))",
         "\"0000\""},
        {"string(xs:hexBinary(" BIN "or(xs:hexBinary('8081'), xs:hexBinary('7F7E'))))",
         "\"FFFF\""},
        {"string(xs:hexBinary(" BIN "xor(xs:hexBinary('1234'), xs:hexBinary('4321'))))",
         "\"5115\""},
        {BIN "xor(xs:hexBinary('00'), ())", ""},
        {BIN "and(xs:hexBinary('00'), xs:hexBinary(''))", "!bin:differing-length-arguments"},
        {"string(xs:hexBinary(" BIN "not(xs:hexBinary('8081'))))", "\"7F7E\""},
        {"(1, 4, 9, 16, -1, -9, -17) ! string(xs:hexBinary(" BIN
         "shift(xs:hexBinary('F00F'), .)))",
         "\"E01E\" \"00F0\" \"1E00\" \"0000\" \"7807\" \"0078\" \"0000\""},
        {"string(xs:hexBinary(" BIN "shift(xs:hexBinary('000001'), 17)))", "\"020000\""},
        {"string(xs:hexBinary(" BIN "shift(xs:hexBinary('F00F'), 99999999999999999999)))",
         "\"0000\""},
        {"(1, 9, 16, 17, -1, -9, -17) ! string(xs:hexBinary(" BIN
         "rotate(xs:hexBinary('F00F'), .)))",
         "\"E01F\" \"1FE0\" \"F00F\" \"E01F\" \"F807\" \"07F8\" \"F807\""},
        {"string(xs:hexBinary(" BIN "rotate(xs:hexBinary('81'), 100000000000000000001)))",
         "\"03\""},
        {BIN "is-bit-set(xs:hexBinary('80'), 0), " BIN "is-bit-set(xs:hexBinary('80'), 7)",
         "true() false()"},
        {BIN "is-bit-set(xs:hexBinary('80'), 8)", "!bin:index-out-of-range"},
        {"string(xs:hexBinary(" BIN "set-bits(xs:hexBinary('00'), (0, 7, 7), true())))",
         "\"81\""},
        {"string(xs:hexBinary(" BIN "set-bits(xs:hexBinary('FF'), (0, 7), false())))", "\"7E\""},
        {BIN "set-bits(xs:hexBinary('FF'), -1, false())", "!bin:index-out-of-range"},
        {BIN "count-bits-set(xs:hexBinary('FFFFFFFF')), " BIN "count-bits-set(())", "32"},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Without the feature the functions do not exist (their D162 place). */
static void test_functions_are_absent_without_the_feature(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.names_feature = false},
                                            &test_fixture) == SXML_STATUS_OK);
    static const char *const absent_case[2] = {BIN "hex('00')", "!XPST0017"};
    expect_query(state, &test_fixture, absent_case);
    sxml_error_record error_record;
    SXML_TEST_EXPECT(state, run_query(&test_fixture,
                                      TEXT("empty(function-lookup(QName('http://expath.org/ns/"
                                           "binary', 'hex'), 1))"),
                                      &error_record) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                TEXT("true()")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_optional_feature_get_name(
                                                      sxml_expath_binary_feature()),
                                                  TEXT("expath-binary")));
}

/* Every allocation may fail: the evaluation then fails cleanly. */
static void test_allocation_failures_leak_nothing(sxml_test_state *state) {
    static const sxml_byte_view expression = SXML_BYTE_VIEW_INITIALIZER(
        BIN "decode-string(" BIN "join((" BIN "encode-string('ab'), " BIN
            "hex('63')))) || " BIN "unpack-integer(" BIN "pack-integer(-123456789123456789123456789, "
            "12), 0, 12) || " BIN "to-base64url(" BIN "rotate(" BIN "hex('F00F'), 3)) || " BIN
            "infer-encoding(" BIN "hex('FEFF'))?encoding");
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; failing_number < 4000 && !has_succeeded;
         failing_number += 1) {
        fixture test_fixture;
        sxml_status status = fixture_create(
            &(fixture_options){.failing_allocation_number = failing_number, .names_feature = true},
            &test_fixture);
        if (status == SXML_STATUS_OK) {
            sxml_error_record error_record;
            status = run_query(&test_fixture, expression, &error_record);
            has_succeeded = status == SXML_STATUS_OK;
            if (has_succeeded) {
                SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                            sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                            TEXT("\"abc-123456789123456789123456789gH8UTF-16BE\"")));
            }
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case binary_test_cases[] = {
    {.name = "constants_and_conversions", .function = test_constants_and_conversions},
    {.name = "basic_operations", .function = test_basic_operations},
    {.name = "text_decoding_and_encoding", .function = test_text_decoding_and_encoding},
    {.name = "packing_and_unpacking", .function = test_packing_and_unpacking},
    {.name = "bitwise_operations", .function = test_bitwise_operations},
    {.name = "functions_are_absent_without_the_feature", .function = test_functions_are_absent_without_the_feature},
    {.name = "allocation_failures_leak_nothing", .function = test_allocation_failures_leak_nothing},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "function_library_binary",
            .cases = binary_test_cases,
            .case_count = SXML_TEST_COUNT(binary_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
