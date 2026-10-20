/*
 * Tests for the number picture engine (src/format_internal.h) and the
 * format part of the function library (src/function_library_format.c,
 * packet 2.9): integer pictures, decimal formats and number pictures
 * through the engine, then fn:format-integer and fn:format-number through
 * the public XPath API, with leak checks and allocation-failure loops
 * (D23). Cases are named by their QT4 test case where they come from one;
 * the decimal-format environments of QT4 are covered here at the engine,
 * since the static context's decimal formats do not reach the evaluator
 * yet (D119).
 *
 * Golden file: tests/memory_test.c, with the public-API fixture of
 * tests/xpath_test.c.
 */
#include "format_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ------------------------------------------------------- Engine fixture */

/* A counting allocator for one engine call; false from finish when a
   block leaked. */
typedef struct engine_fixture {
    sxml_counting_allocator *counting_allocator;
} engine_fixture;

static sxml_status engine_fixture_create(uint64_t failing_allocation_number,
                                         engine_fixture *fixture) {
    fixture->counting_allocator = NULL;
    return sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &fixture->counting_allocator);
}

static bool engine_fixture_destroy(engine_fixture *fixture) {
    bool has_no_leak = true;
    if (fixture->counting_allocator != NULL) {
        has_no_leak = sxml_counting_allocator_read_counts(fixture->counting_allocator)
                          .live_allocation_count == 0;
        sxml_counting_allocator_destroy(fixture->counting_allocator);
    }
    return has_no_leak;
}

/* ---------------------------------------------------- Integer pictures */

/* Formats value (decimal digits, optional "-") by picture with the engine;
   true when it gives expected. */
typedef struct integer_case {
    const char *value;
    const char *picture;
    const char *expected;
} integer_case;

static bool integer_case_holds(integer_case test_case) {
    engine_fixture fixture;
    if (engine_fixture_create(0, &fixture) != SXML_STATUS_OK) {
        return false;
    }
    sxml_integer_picture picture;
    bool holds = sxml_integer_picture_parse(text_view(test_case.picture), &picture) ==
                 SXML_STATUS_OK;
    sxml_format_text text = {
        .allocator = sxml_counting_allocator_as_allocator(fixture.counting_allocator),
        .bytes = NULL,
        .byte_count = 0,
        .byte_capacity = 0};
    const bool is_negative = test_case.value[0] == '-';
    if (holds) {
        holds = sxml_format_integer(
                    &(sxml_integer_formatting){
                        .picture = &picture,
                        .magnitude_digits = text_view(test_case.value + (is_negative ? 1 : 0)),
                        .is_negative = is_negative},
                    &text) == SXML_STATUS_OK &&
                sxml_byte_view_equals(sxml_format_text_view(&text), text_view(test_case.expected));
        if (!holds) {
            (void)fprintf(stderr, "format-integer(%s, '%s') gave '%.*s', not '%s'\n",
                          test_case.value, test_case.picture, (int)text.byte_count,
                          text.bytes == NULL ? "" : (const char *)text.bytes, test_case.expected);
        }
    }
    sxml_format_text_release(&text);
    return engine_fixture_destroy(&fixture) && holds;
}

static void test_integer_pictures_format(sxml_test_state *state) {
    static const integer_case cases[] = {
        {"123", "0000", "0123"},
        {"-123", "99999", "-00123"},
        {"0", "000", "000"},
        {"1500000", "0,000", "1,500,000"},
        {"1500000", "#,###,000", "1,500,000"},
        {"1500000", "#", "1500000"},
        {"1500000", "#a", "1500000"},
        {"1500000", "###\xF0\x90\x84\x80" "0,00", "1500\xF0\x90\x84\x80" "0,00"},
        {"602347826", "#(000)000-000", "602)347-826"},
        {"12345678901", "# 000", "12 345 678 901"},
        {"123456789", "0,0,00,0", "12345,6,78,9"},
        {"123456789", "00,00,00", "1,23,45,67,89"},
        {"123456789", "000,00,00", "12345,67,89"},
        {"1234", "#;##1;", "1;234"},
        {"1234", "#,\xF0\x90\x92\xA0\xF0\x90\x92\xA0\xF0\x90\x92\xA0",
         "\xF0\x90\x92\xA1,\xF0\x90\x92\xA2\xF0\x90\x92\xA3\xF0\x90\x92\xA4"},
        {"5", "\xD9\xA0\xD9\xA0", "\xD9\xA0\xD9\xA5"},
        {"7", "a", "g"},
        {"27", "a", "aa"},
        {"2147483648", "a", "fxshrxx"},
        {"-5", "a", "-e"},
        {"0", "a", "0"},
        /* Greek alphabets (packet 4.17): final sigma counts in lower case,
           upper case skips U+03A2. */
        {"18", "\xCE\xB1", "\xCF\x82"},
        {"26", "\xCE\xB1", "\xCE\xB1\xCE\xB1"},
        {"18", "\xCE\x91", "\xCE\xA3"},
        {"25", "\xCE\x91", "\xCE\x91\xCE\x91"},
        /* Symbol sequences: CIRCLED DIGIT ONE over 0 to 50 in four runs,
           DIGIT ONE FULL STOP from DIGIT ZERO FULL STOP, decimal outside. */
        {"0", "\xE2\x91\xA0", "\xE2\x93\xAA"},
        {"20", "\xE2\x91\xA0", "\xE2\x91\xB3"},
        {"21", "\xE2\x91\xA0", "\xE3\x89\x91"},
        {"50", "\xE2\x91\xA0", "\xE3\x8A\xBF"},
        {"51", "\xE2\x91\xA0", "51"},
        {"-3", "\xE2\x91\xA0", "-\xE2\x91\xA2"},
        {"0", "\xE2\x92\x88", "\xF0\x9F\x84\x80"},
        {"0", "\xE2\x91\xB4", "0"},
        {"11", "\xE2\x9D\xB6", "\xE2\x93\xAB"},
        {"10", "\xF0\x9D\x8D\xA0", "10"},
        /* Japanese ideographic numerals (IDEOGRAPH ONE): no 一 before
           十 百 千, no zero digit, myriad units (QT4 format-integer-052). */
        {"0", "\xE4\xB8\x80", "\xE3\x80\x87"},
        {"11", "\xE4\xB8\x80", "\xE5\x8D\x81\xE4\xB8\x80"},
        {"302", "\xE4\xB8\x80", "\xE4\xB8\x89\xE7\x99\xBE\xE4\xBA\x8C"},
        {"10001", "\xE4\xB8\x80", "\xE4\xB8\x80\xE4\xB8\x87\xE4\xB8\x80"},
        {"-20", "\xE4\xB8\x80", "-\xE4\xBA\x8C\xE5\x8D\x81"},
        {"57", "I", "LVII"},
        {"1999", "i", "mcmxcix"},
        {"4000", "I", "4000"},
        {"0", "Ww", "Zero"},
        {"123", "w", "one hundred and twenty-three"},
        {"1005", "w", "one thousand and five"},
        {"1000001", "W", "ONE MILLION AND ONE"},
        {"21", "Ww", "Twenty-one"},
        {"100", "w;o", "one hundredth"},
        {"0", "w;o", "zeroth"},
        {"2", "W;o", "SECOND"},
        {"-5", "Ww;o", "-Fifth"},
        {"20", "w;o", "twentieth"},
        {"112", "w;o", "one hundred and twelfth"},
        {"21", "1;o", "21st"},
        {"12", "1;o", "12th"},
        {"-85", "1;o", "-85th"},
        {"-8500", "#,##1;o", "-8,500th"},
        {"1234", "()Ww;o", "1234th"},
        {"1234", "\xEF\xAF\xB4", "1234"},
        {"1234", "16^xxxx", "04d2"},
        {"1234", "16^X", "4D2"},
        {"12345678", "16^xxxx_xxxx", "00bc_614e"},
        {"12345678", "16^#_xxxx", "bc_614e"},
        {"255", "2^xxxx xxxx", "1111 1111"},
        {"1023", "32^XXXX", "00VV"},
        {"1023", "10^XXXX", "1023"},
        {"1023", "10^00", "10^23"},
        {"-12", "16^XX", "-0C"},
        {"999", "8^#:#:#:#:XX", "1:7:47"},
        {"999", "10^0000", "00^0999"},
        {"65535", "16^xx^xx", "ff^ff"},
        {"21", "16^xx;o", "15st"},
        {"1", "Ww;o(-en)a", "First"},
        {"123456789012345678901234567890", "#,##0",
         "123,456,789,012,345,678,901,234,567,890"},
        {"123456789012345678901234567890", "36^X", "BYW97UM9S91DLZ68TSI"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, integer_case_holds(cases[case_index]));
    }
}

static void test_invalid_integer_pictures_are_rejected(sxml_test_state *state) {
    static const char *const pictures[] = {
        "",         ";",           "0,000,",    "11#0,000",    ",123",  "0,00,,000",
        "1;o(-er)z", "Ww;o()(",    "123\xD9\xA1", "0#",        "1o",    "Ww;o(",
        "16^xX",    "16^x1",       "1;x",       "Ww;co",
    };
    for (size_t picture_index = 0; picture_index < SXML_TEST_COUNT(pictures);
         picture_index += 1) {
        sxml_integer_picture picture;
        SXML_TEST_EXPECT(state, sxml_integer_picture_parse(text_view(pictures[picture_index]),
                                                           &picture) ==
                                    SXML_STATUS_MALFORMED_INPUT);
    }
    sxml_integer_picture picture;
    SXML_TEST_EXPECT(state,
                     sxml_integer_picture_parse(TEXT("1;o(x)a"), &picture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, picture.is_ordinal &&
                                sxml_byte_view_equals(picture.numbering_variant, TEXT("x")));
    SXML_TEST_EXPECT(state, sxml_integer_picture_parse_primary_format_token(TEXT("001;"),
                                                                            &picture) ==
                                SXML_STATUS_MALFORMED_INPUT);
}

/* ------------------------------------------------------ Decimal formats */

/* Applies properties to the defaults. */
static sxml_status apply_properties(const sxml_decimal_format_property *properties,
                                    size_t property_count,
                                    sxml_decimal_format_settings *settings) {
    const sxml_decimal_format_settings defaults = sxml_decimal_format_default_settings();
    return sxml_decimal_format_apply(
        &(sxml_decimal_format_change){
            .base_settings = &defaults,
            .decimal_format_properties = {.decimal_format_properties = properties,
                                          .decimal_format_property_count = property_count}},
        settings);
}

#define PROPERTY(kind, text)                                                                   \
    {.property_kind = SXML_DECIMAL_FORMAT_PROPERTY_##kind,                                     \
     .property_value = SXML_BYTE_VIEW_INITIALIZER(text)}

static void test_decimal_formats_apply_and_check_properties(sxml_test_state *state) {
    const sxml_decimal_format_property renditions[] = {
        PROPERTY(PERCENT, "%:pc"), PROPERTY(GROUPING_SEPARATOR, ",:::"),
        PROPERTY(EXPONENT_SEPARATOR, "^:\xC3\x97" "10^"), PROPERTY(MINUS_SIGN, "minus "),
        PROPERTY(DECIMAL_SEPARATOR, ".:")};
    sxml_decimal_format_settings settings;
    SXML_TEST_EXPECT(state, apply_properties(renditions, SXML_TEST_COUNT(renditions),
                                             &settings) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, settings.percent.marker == '%' &&
                                sxml_byte_view_equals(settings.percent.rendition, TEXT("pc")));
    SXML_TEST_EXPECT(state, settings.grouping_separator.marker == ',' &&
                                sxml_byte_view_equals(settings.grouping_separator.rendition,
                                                      TEXT("::")));
    SXML_TEST_EXPECT(state, settings.exponent_separator.marker == '^');
    SXML_TEST_EXPECT(state, settings.decimal_separator.rendition.byte_count == 0);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(settings.minus_sign, TEXT("minus ")));

    /* QT4 numberformat-504 to -509 and -564. */
    static const sxml_decimal_format_property invalid[][2] = {
        {PROPERTY(DECIMAL_SEPARATOR, "|"), PROPERTY(GROUPING_SEPARATOR, "|")},
        {PROPERTY(DECIMAL_SEPARATOR, "...."), PROPERTY(NAN, "x")},
        {PROPERTY(ZERO_DIGIT, "$"), PROPERTY(NAN, "x")},
        {PROPERTY(ZERO_DIGIT, "\xD9\xA1"), PROPERTY(NAN, "x")},
        {PROPERTY(DIGIT, "7"), PROPERTY(NAN, "x")},
        {PROPERTY(PATTERN_SEPARATOR, "!!"), PROPERTY(NAN, "x")},
        {PROPERTY(PERCENT, ""), PROPERTY(NAN, "x")},
        {PROPERTY(PER_MILLE, "e"), PROPERTY(NAN, "x")},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(invalid); case_index += 1) {
        SXML_TEST_EXPECT(state, apply_properties(invalid[case_index], 2, &settings) ==
                                    SXML_STATUS_MALFORMED_INPUT);
    }
    const sxml_decimal_format_property arabic[] = {PROPERTY(ZERO_DIGIT, "\xD9\xA0")};
    SXML_TEST_EXPECT(state, apply_properties(arabic, 1, &settings) == SXML_STATUS_OK &&
                                settings.zero_digit == 0x660U);
}

/* ------------------------------------------------------ Number pictures */

/* A number formatted by the engine with properties applied. */
typedef struct number_case {
    const char *value;
    const char *picture;
    const char *expected;
    const sxml_decimal_format_property *properties;
    size_t property_count;
} number_case;

/* Writes value ("-1.5", "NaN", "INF", "-INF") by the picture; the value
   text is the magnitude for the engine. */
static sxml_status format_number_case(number_case test_case, sxml_allocator allocator,
                                      sxml_format_text *text) {
    sxml_decimal_format_settings settings;
    sxml_status status =
        apply_properties(test_case.properties, test_case.property_count, &settings);
    sxml_number_picture picture;
    if (status == SXML_STATUS_OK) {
        status = sxml_number_picture_parse(
            &(sxml_number_picture_request){.picture_string = text_view(test_case.picture),
                                           .settings = &settings},
            &picture);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_negative = test_case.value[0] == '-';
    const char *magnitude = test_case.value + (is_negative ? 1 : 0);
    const sxml_number_sign sign = is_negative ? SXML_NUMBER_SIGN_NEGATIVE : SXML_NUMBER_SIGN_POSITIVE;
    const uint32_t multiplier = sxml_number_picture_get_multiplier(&picture, sign);
    *text = (sxml_format_text){.allocator = allocator, .bytes = NULL, .byte_count = 0,
                               .byte_capacity = 0};
    return sxml_format_number(
        &(sxml_number_formatting){
            .picture = &picture,
            .number_class = strcmp(magnitude, "NaN") == 0   ? SXML_NUMBER_CLASS_NOT_A_NUMBER
                            : strcmp(magnitude, "INF") == 0 ? SXML_NUMBER_CLASS_INFINITE
                                                            : SXML_NUMBER_CLASS_FINITE,
            .sign = sign,
            .magnitude_text = text_view(magnitude),
            .decimal_point_shift = multiplier == 100U ? 2U : multiplier == 1000U ? 3U : 0U},
        text);
}

static bool number_case_holds(number_case test_case) {
    engine_fixture fixture;
    if (engine_fixture_create(0, &fixture) != SXML_STATUS_OK) {
        return false;
    }
    sxml_format_text text = {.allocator = {.state = NULL, .operations = NULL},
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
    const sxml_status status = format_number_case(
        test_case, sxml_counting_allocator_as_allocator(fixture.counting_allocator), &text);
    const bool holds =
        status == SXML_STATUS_OK &&
        sxml_byte_view_equals(sxml_format_text_view(&text), text_view(test_case.expected));
    if (!holds) {
        (void)fprintf(stderr, "format-number(%s, '%s') gave status %d '%.*s', not '%s'\n",
                      test_case.value, test_case.picture, (int)status, (int)text.byte_count,
                      text.bytes == NULL ? "" : (const char *)text.bytes, test_case.expected);
    }
    sxml_format_text_release(&text);
    return engine_fixture_destroy(&fixture) && holds;
}

#define NUMBER(value, picture, expected) {(value), (picture), (expected), NULL, 0}
#define NUMBER_WITH(value, picture, expected, properties)                                      \
    {(value), (picture), (expected), (properties), SXML_TEST_COUNT(properties)}

static const sxml_decimal_format_property bar_and_dot[] = {PROPERTY(DECIMAL_SEPARATOR, "|"),
                                                           PROPERTY(GROUPING_SEPARATOR, ".")};
static const sxml_decimal_format_property bang_and_backslash[] = {
    PROPERTY(DIGIT, "!"), PROPERTY(PATTERN_SEPARATOR, "\\")};
static const sxml_decimal_format_property off_the_scale[] = {
    PROPERTY(INFINITY, "off-the-scale")};
static const sxml_decimal_format_property per_mille_m[] = {PROPERTY(PER_MILLE, "m")};
static const sxml_decimal_format_property underscore_minus[] = {PROPERTY(MINUS_SIGN, "_")};
static const sxml_decimal_format_property comma_and_space[] = {
    PROPERTY(DECIMAL_SEPARATOR, ","), PROPERTY(GROUPING_SEPARATOR, " ")};
static const sxml_decimal_format_property percent_c[] = {PROPERTY(PERCENT, "c")};
static const sxml_decimal_format_property arabic_digits[] = {
    PROPERTY(DIGIT, "!"), PROPERTY(ZERO_DIGIT, "\xD9\xA0")};
static const sxml_decimal_format_property osmanya_zero[] = {
    PROPERTY(ZERO_DIGIT, "\xF0\x90\x92\xA0")};
static const sxml_decimal_format_property not_a_number[] = {PROPERTY(NAN, "non-numeric")};
static const sxml_decimal_format_property percent_rendition[] = {PROPERTY(PERCENT, "%:pc")};
static const sxml_decimal_format_property exponent_rendition[] = {
    PROPERTY(EXPONENT_SEPARATOR, "^:\xC3\x97" "10^")};
static const sxml_decimal_format_property german[] = {PROPERTY(DECIMAL_SEPARATOR, ","),
                                                      PROPERTY(GROUPING_SEPARATOR, "."),
                                                      PROPERTY(EXPONENT_SEPARATOR, "^")};
static const sxml_decimal_format_property fortran[] = {PROPERTY(EXPONENT_SEPARATOR, "E")};
static const sxml_decimal_format_property long_separators[] = {
    PROPERTY(DECIMAL_SEPARATOR, ".: \xC2\xA4 "), PROPERTY(GROUPING_SEPARATOR, ",:::")};
static const sxml_decimal_format_property per_mille_p[] = {PROPERTY(PER_MILLE, "p:pm")};
static const sxml_decimal_format_property bang_pattern[] = {PROPERTY(PATTERN_SEPARATOR, "!")};
static const sxml_decimal_format_property infinity_inf[] = {PROPERTY(INFINITY, "INF")};

static void test_number_pictures_format(sxml_test_state *state) {
    const number_case cases[] = {
        NUMBER("87504.4812", "000,000.000000", "087,504.481200"),
        NUMBER("-87504.4812", "000,000.000###;###,###.000###", "87,504.4812"),
        NUMBER("0.4857", "###.###%", "48.57%"),
        NUMBER("0.4857", "###.###\xE2\x80\xB0", "485.7\xE2\x80\xB0"),
        NUMBER("95.4857", "\xC2\xA4###.####", "\xC2\xA4" "95.4857"),
        NUMBER("185.2812", "PREFIX##00.000###SUFFIX", "PREFIX185.2812SUFFIX"),
        NUMBER_WITH("931.4857", "000.000|###", "000.931|486", bar_and_dot),
        NUMBER_WITH("26931.4", "+!!!,!!!.!!!\\-!!,!!!.!!!", "+26,931.4", bang_and_backslash),
        NUMBER_WITH("-26931.4", "+!!,!!!.!!!\\-!!!,!!!.!!!", "-26,931.4", bang_and_backslash),
        NUMBER_WITH("-26931.4", "!!!,!!!.!!!", "-26,931.4", bang_and_backslash),
        NUMBER_WITH("INF", "#####", "off-the-scale", off_the_scale),
        NUMBER_WITH("NaN", "#####", "non-numeric", not_a_number),
        NUMBER_WITH("0.4857", "###.###m", "485.7m", per_mille_m),
        NUMBER_WITH("-26931.4", "###,###.###", "_26,931.4", underscore_minus),
        NUMBER_WITH("-26931.4", "zzz-###,###.###", "_zzz-26,931.4", underscore_minus),
        NUMBER_WITH("7654321.4857", "### ### ###,#####", "7 654 321,4857", comma_and_space),
        NUMBER_WITH("0.4857", "###.###c", "48.57c", percent_c),
        NUMBER_WITH("4030201.0506",
                    "#!!!,!!!,\xD9\xA0\xD9\xA0\xD9\xA0.\xD9\xA0\xD9\xA0\xD9\xA0\xD9\xA0\xD9\xA0"
                    "\xD9\xA0" "0",
                    "#\xD9\xA4,\xD9\xA0\xD9\xA3\xD9\xA0,\xD9\xA2\xD9\xA0\xD9\xA1.\xD9\xA0\xD9\xA5"
                    "\xD9\xA0\xD9\xA6\xD9\xA0\xD9\xA0" "0",
                    arabic_digits),
        NUMBER_WITH("1.0", "\xF0\x90\x92\xA0.\xF0\x90\x92\xA0" "e\xF0\x90\x92\xA0",
                    "\xF0\x90\x92\xA1.\xF0\x90\x92\xA0" "e\xF0\x90\x92\xA0", osmanya_zero),
        NUMBER_WITH("0.14", "01%", "14pc", percent_rendition),
        NUMBER_WITH("12345", "0.0###^0", "1.2345\xC3\x97" "10^4", exponent_rendition),
        NUMBER_WITH("1234.5678", "#.##0,00", "1.234,57", german),
        NUMBER_WITH("12345", "0,###^0", "1,234^4", german),
        NUMBER_WITH("1234.5678", "00.000E0", "12.346E2", fortran),
        NUMBER_WITH("0.234", "0.0E0", "2.3E-1", fortran),
        NUMBER_WITH("0.234", "#.00E0", "0.23E0", fortran),
        NUMBER_WITH("0.234", ".00E0", ".23E0", fortran),
        NUMBER_WITH("123456789.987654321", "###,###,###.999,999,999",
                    "123::456::789 \xC2\xA4 987::654::321", long_separators),
        NUMBER_WITH("0.25", "0p", "250pm", per_mille_p),
        NUMBER_WITH("-5", "0!(0)", "(5)", bang_pattern),
        NUMBER_WITH("-INF", "PREFIX#SUFFIX", "-PREFIXINFSUFFIX", infinity_inf),
        NUMBER_WITH("-INF", "PREFIX#SUFFIX;prefix#suffix", "prefixINFsuffix", infinity_inf),
        NUMBER("987654321", "###,##0,00.00", "9876,543,21.00"),
        NUMBER("239236.588", "00000.00", "239236.59"),
        NUMBER("12345.678", "9.9999e999", "1.2346e004"),
        NUMBER("12345.678", "9.9999eDog", "12345.6780eDog"),
        NUMBER("12345.678", "end9.9999e99end", "end1.2346e04end"),
        NUMBER("0.05", "9.9999e99", "5.0000e-02"),
        NUMBER("0.99999999", ".#e0", "1.0e0"),
        NUMBER("0.99999999", "#.#e0", "1e0"),
        NUMBER("0.99999999", "0.0e0", "10.0e-1"),
        NUMBER("1.2", "#e0", "0.1e1"),
        NUMBER("0.2", "000.0e0", "200.0e-3"),
        NUMBER("0", "#.#e9", "0e0"),
        NUMBER("-0", "0.0e0", "-0.0e0"),
        NUMBER("0", "#.#", ".0"),
        NUMBER("0", "#.", "0"),
        NUMBER("0", "#.00", ".00"),
        NUMBER("0.2", "#.suffix", "0suffix"),
        NUMBER("12345.678", "9.", "12346"),
        NUMBER("12345.6789012345", "#.#,##,#", "12345.6,78,9"),
        NUMBER("12345.6789012345", "##,##.###,###,###", "1,23,45.678,901,234"),
        NUMBER("642120", "####,##", "6421,20"),
        NUMBER("642120", "#,##", "64,21,20"),
        NUMBER("642120", "##,#,#", "6421,2,0"),
        NUMBER("642120", "#,#,#", "6,4,2,1,2,0"),
        NUMBER("3000000", ",##0", "3,000,000"),
        NUMBER("123456789", "####,###,##,0", "123,456,78,9"),
        NUMBER("0.9952832031249997", "###,##0.00", "1.00"),
        NUMBER("2.5", "0", "2"),
        NUMBER("3.5", "0", "4"),
        NUMBER("1.10203040506070809", "0.00000000000000000%", "110.20304050607080900%"),
        NUMBER("0.0001", "0", "0"),
        NUMBER("0.6", "#", "1"),
        NUMBER("9.99", "0.0", "10.0"),
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, number_case_holds(cases[case_index]));
    }
}

static void test_invalid_number_pictures_are_rejected(sxml_test_state *state) {
    static const char *const pictures[] = {
        "9.9999E999", "9.99e99e99", "9.9999e999%", "9.9999e,", ".e99",   "#,.##",
        "#.,##",      "#,",         "#,,###",      "#.#,,#",   "000.##0", "fred.ginger",
        "0;0;0",      "#.#.#",      "0%%",         "0%\xE2\x80\xB0", "0#", "#0%0",
        "",           "abc",
    };
    const sxml_decimal_format_settings settings = sxml_decimal_format_default_settings();
    for (size_t picture_index = 0; picture_index < SXML_TEST_COUNT(pictures);
         picture_index += 1) {
        sxml_number_picture picture;
        const sxml_status status = sxml_number_picture_parse(
            &(sxml_number_picture_request){.picture_string = text_view(pictures[picture_index]),
                                           .settings = &settings},
            &picture);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_MALFORMED_INPUT);
        if (status != SXML_STATUS_MALFORMED_INPUT) {
            (void)fprintf(stderr, "picture '%s' was accepted\n", pictures[picture_index]);
        }
    }
}

static void test_engine_survives_every_allocation_failure(sxml_test_state *state) {
    const number_case test_case = NUMBER_WITH(
        "123456789.987654321", "###,###,###.999,999,999",
        "123::456::789 \xC2\xA4 987::654::321", long_separators);
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 1000; failing_number += 1) {
        engine_fixture fixture;
        status = engine_fixture_create(failing_number, &fixture);
        sxml_format_text text = {.allocator = {.state = NULL, .operations = NULL},
                                 .bytes = NULL,
                                 .byte_count = 0,
                                 .byte_capacity = 0};
        if (status == SXML_STATUS_OK) {
            status = format_number_case(
                test_case, sxml_counting_allocator_as_allocator(fixture.counting_allocator),
                &text);
            if (status == SXML_STATUS_OK) {
                SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_format_text_view(&text),
                                                              text_view(test_case.expected)));
            }
        }
        sxml_format_text_release(&text);
        SXML_TEST_EXPECT(state, engine_fixture_destroy(&fixture));
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 2);
}

/* ------------------------------------------------------- Public API */

typedef struct api_fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
} api_fixture;

static sxml_allocator api_allocator(const api_fixture *fixture) {
    return sxml_counting_allocator_as_allocator(fixture->counting_allocator);
}

static sxml_status api_fixture_create(uint64_t failing_allocation_number, api_fixture *fixture) {
    *fixture = (api_fixture){.counting_allocator = NULL, .processor = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &fixture->counting_allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_create(
            api_allocator(fixture),
            &(sxml_processor_configuration){.allocator = api_allocator(fixture)},
            &fixture->processor);
    }
    return status;
}

/* Destroys the fixture; true when no cell and no block leaked. */
static bool api_fixture_destroy(api_fixture *fixture) {
    bool has_no_leak = true;
    if (fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(fixture->processor);
    if (fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(fixture->counting_allocator);
    }
    return has_no_leak;
}

/* An expression whose value is one xs:string, or the error it raises. */
typedef struct api_case {
    const char *expression_text;
    const char *expected;
    const char *expected_error_code;
} api_case;

/* Compiles and evaluates the expression; on success *matches tells
   whether the value is the one xs:string expected. */
typedef struct api_outcome {
    sxml_error_record error_record;
    bool matches;
} api_outcome;

static sxml_status run_api_case(const api_fixture *fixture, api_case test_case,
                                api_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        api_allocator(fixture),
        &(sxml_xpath_expression_configuration){.processor = fixture->processor,
                                               .expression_text =
                                                   text_view(test_case.expression_text),
                                               .source_name = TEXT("format.xpath"),
                                               .error_record = &outcome->error_record},
        &expression);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression, &(sxml_xpath_evaluation_request){.error_record = &outcome->error_record},
            &result);
    }
    sxml_xpath_expression_destroy(expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    outcome->matches =
        test_case.expected != NULL &&
        sxml_sequence_get_representation(result) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_atomic_item_get_datum(result.content, &datum) == SXML_STATUS_OK &&
        datum.datum_kind == SXML_DATUM_KIND_STRING &&
        sxml_byte_view_equals(datum.datum_details.string.utf8, text_view(test_case.expected));
    if (!outcome->matches && test_case.expected != NULL) {
        (void)fprintf(stderr, "%s: wrong value\n", test_case.expression_text);
        if (datum.datum_kind == SXML_DATUM_KIND_STRING) {
            (void)fprintf(stderr, "  gave '%.*s'\n", (int)datum.datum_details.string.utf8.byte_count,
                          (const char *)datum.datum_details.string.utf8.bytes);
        }
    }
    sxml_sequence_release(result);
    return status;
}

static bool api_case_holds(api_case test_case) {
    api_fixture fixture;
    bool holds = false;
    if (api_fixture_create(0, &fixture) == SXML_STATUS_OK) {
        api_outcome outcome;
        const sxml_status status = run_api_case(&fixture, test_case, &outcome);
        if (test_case.expected_error_code != NULL) {
            holds = status != SXML_STATUS_OK &&
                    sxml_byte_view_equals(outcome.error_record.code.local_name,
                                          text_view(test_case.expected_error_code));
            if (!holds) {
                (void)fprintf(stderr, "%s: expected %s, gave status %d '%.*s'\n",
                              test_case.expression_text, test_case.expected_error_code,
                              (int)status, (int)outcome.error_record.code.local_name.byte_count,
                              outcome.error_record.code.local_name.bytes == NULL
                                  ? ""
                                  : (const char *)outcome.error_record.code.local_name.bytes);
            }
        } else {
            holds = status == SXML_STATUS_OK && outcome.matches;
            if (status != SXML_STATUS_OK) {
                (void)fprintf(stderr, "%s: status %d '%.*s'\n", test_case.expression_text,
                              (int)status, (int)outcome.error_record.code.local_name.byte_count,
                              outcome.error_record.code.local_name.bytes == NULL
                                  ? ""
                                  : (const char *)outcome.error_record.code.local_name.bytes);
            }
        }
    }
    return api_fixture_destroy(&fixture) && holds;
}

#define VALUE(expression, expected) {(expression), (expected), NULL}
#define ERROR(expression, code) {(expression), NULL, (code)}

static void test_format_integer_through_the_api(sxml_test_state *state) {
    static const api_case cases[] = {
        VALUE("format-integer(123, '00001')", "00123"),
        VALUE("format-integer((), 'Ww')", ""),
        VALUE("string-join(for $i in 1 to 4 return format-integer($i, 'A'))", "ABCD"),
        VALUE("string-join(for $i in 1 to 10 return format-integer($i, 'Ww'), '|')",
              "One|Two|Three|Four|Five|Six|Seven|Eight|Nine|Ten"),
        VALUE("format-integer(11, 'Ww', 'en')", "Eleven"),
        VALUE("format-integer(11, 'Ww', '@*!+%')", "Eleven"),
        VALUE("format-integer(-2147483649, '1')", "-2147483649"),
        VALUE("format-integer(9007199254740993, '0')", "9007199254740993"),
        VALUE("format-integer(999999999999999999, '#,##0')", "999,999,999,999,999,999"),
        VALUE("format-integer(4294967297, 'a')", "mwlqkww"),
        VALUE("format-integer(-9223372036854775808, '1')", "-9223372036854775808"),
        VALUE("format-integer(123456789012345678901234567890, '1')",
              "123456789012345678901234567890"),
        VALUE("string((every $r in (2 to 36), $n in (0, 1, 7, 23, 86, 159, 2134672, "
              "105093381562) satisfies (format-integer($n, $r || '^X') => parse-integer($r)) "
              "eq $n))",
              "true"),
        ERROR("format-integer(1, '')", "FODF1310"),
        ERROR("format-integer(1500000, '0,000,')", "FODF1310"),
        ERROR("format-integer(-1, '1o')", "FODF1310"),
        ERROR("format-integer(1234, 'Ww;o()(')", "FODF1310"),
        ERROR("format-integer(1.5, '1')", "XPTY0004"),
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, api_case_holds(cases[case_index]));
    }
}

static void test_format_number_through_the_api(sxml_test_state *state) {
    static const api_case cases[] = {
        VALUE("format-number(2392.14*36.58,'000,000.000000')", "087,504.481200"),
        VALUE("format-number(12792.14*96.58,'##,###,000.000###')", "1,235,464.8812"),
        VALUE("format-number(0.4857,'###.###%')", "48.57%"),
        VALUE("format-number(1 div 0e0,'#####')", "Infinity"),
        VALUE("format-number(-1 div 0e0,'#####')", "-Infinity"),
        VALUE("format-number((),'###.###')", "NaN"),
        VALUE("format-number(number('abc'),'PREFIX#SUFFIX')", "NaN"),
        VALUE("format-number(0 div xs:double('-INF'), '#')", "-0"),
        VALUE("format-number(1E25,'#,######')", "10,000000,000000,000000,000000"),
        VALUE("format-number(1E100 div 3,'#####################')",
              "3333333333333333000000000000000000000000000000000000000000000000000000000000000000"
              "000000000000000000"),
        VALUE("format-number(000123456789012345678901234567890.123456789012345678900000, "
              "'##0.0####################################################')",
              "123456789012345678901234567890.1234567890123456789"),
        VALUE("string-join((format-number(0, '#'), format-number(0.0, '#'), "
              "format-number(0.0e0, '#'), format-number(xs:float(0), '#')), '|')",
              "0|0|0|0"),
        VALUE("format-number(0.4857,'###.###%', ())", "48.57%"),
        VALUE("format-number(xs:double('1e308'), '0%')", "Infinity%"),
        VALUE("format-number(xs:double('-1e308'), '0%')", "-Infinity%"),
        VALUE("format-number(xs:decimal('79228162514264337593543950335'), '0%')",
              "7922816251426433759354395033500%"),
        VALUE("format-number(xs:float('0.1'), '0.0')", "0.1"),
        VALUE("format-number(xs:float('3.4028235E38'), '0.0e0')", "3.4e38"),
        /* QT4 fn-format-number-limits-007 expects "4.9e-324", Java's
           digits; the shortest decimal (section 4.9.5) is 5e-324. */
        VALUE("format-number(4.9E-324, '0.0e0')", "5.0e-324"),
        VALUE("format-number(1.7976931348623157E308, '0.0e0')", "1.8e308"),
        VALUE("format-number(0.123456789012345678, '0.000000000000000000')",
              "0.123456789012345678"),
        VALUE("format-number(12345.678, '9.9999e999')", "1.2346e004"),
        VALUE("format-number(-0.00012345678, '9.99e99')", "-1.23e-04"),
        VALUE("format-number(1.1, '0|0', options := map { 'decimal-separator': '|' })", "1|1"),
        VALUE("format-number(1234.56, '#.##0,#', options := map { 'decimal-separator': [','], "
              "'grouping-separator': ['.'] })",
              "1.234,6"),
        VALUE("format-number(-1234.56, '#,##0.#', options := {'minus-sign': 'minus '})",
              "minus 1,234.6"),
        VALUE("format-number(0.15, '0%', options := {'percent': '%: per cent'})",
              "15 per cent"),
        VALUE("format-number(2.15, '9.99', {'decimal-separator': '.: \xC2\xA4 '})",
              "2 \xC2\xA4 15"),
        VALUE("format-number(1234.5, '0.0x0', { 'exponent-separator': 'x:EXP' })", "1.2EXP3"),
        VALUE("string-join((xs:double('INF'), xs:double('-INF')) ! format-number(., '0', "
              "{ 'infinity': 'inf' }), ' ')",
              "inf -inf"),
        VALUE("format-number(0.25, '0\xE2\x80\xB0', { 'per-mille': 'p' })", "0\xE2\x80\xB0"),
        VALUE("string-join((5, -5) ! format-number(., '0!(0)', { 'pattern-separator': '!' }), "
              "' ')",
              "5 (5)"),
        VALUE("format-number(1, '0', {xs:QName('xs:other'): 1})", "1"),
        ERROR("format-number(1, '0', {'decimal-separator': '|', 'grouping-separator': '|'})",
              "FODF1290"),
        ERROR("format-number(1, '0', { 'zero-digit':'$' })", "FODF1290"),
        ERROR("format-number(1, '0', { 'pattern-separator': '!!' })", "FODF1290"),
        ERROR("format-number(1, '0', { 'colour': 'red' })", "XPTY0004"),
        ERROR("format-number(1, '0', { 'format-name': 1 })", "XPTY0004"),
        ERROR("format-number(1, '0', { 'format-name': 'myminus' })", "FODF1280"),
        ERROR("format-number(931.45, '000.##0', 'foo:bar')", "FODF1280"),
        ERROR("format-number(12345.678, '9.9999E999', 'BadFormat')", "FODF1280"),
        ERROR("format-number(931.4857,'000.##0')", "FODF1310"),
        ERROR("format-number(12345.678, '#,,###')", "FODF1310"),
        ERROR("format-number('abc','000.##0')", "XPTY0004"),
        ERROR("format-number(931.45, 931.45)", "XPTY0004"),
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, api_case_holds(cases[case_index]));
    }
}

static void test_functions_survive_every_allocation_failure(sxml_test_state *state) {
    const api_case test_case = VALUE(
        "format-integer(123456789012345678901234567890, '16^xxxx_xxxx;o') || ' ' || "
        "format-number(1234.5678e0, '#.##0,00%', {'percent': '%:pc', 'grouping-separator': "
        "'.', 'decimal-separator': ','}) || ' ' || format-integer(1234, 'Ww;o')",
        "1_8ee9_0ff6_c373_e0ee_4e3f_0ad2th 123.456,78pc One Thousand Two Hundred and "
        "Thirty-fourth");
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        api_fixture fixture;
        status = api_fixture_create(failing_number, &fixture);
        if (status == SXML_STATUS_OK) {
            api_outcome outcome;
            status = run_api_case(&fixture, test_case, &outcome);
            if (status == SXML_STATUS_OK) {
                SXML_TEST_EXPECT(state, outcome.matches);
            } else if (status != SXML_STATUS_OUT_OF_MEMORY) {
                (void)fprintf(stderr, "error %.*s\n",
                              (int)outcome.error_record.code.local_name.byte_count,
                              (const char *)outcome.error_record.code.local_name.bytes);
            }
        }
        SXML_TEST_EXPECT(state, api_fixture_destroy(&fixture));
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            (void)fprintf(stderr, "allocation %llu: status %d\n",
                          (unsigned long long)failing_number, (int)status);
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case format_test_cases[] = {
    {.name = "integer_pictures_format", .function = test_integer_pictures_format},
    {.name = "invalid_integer_pictures_are_rejected",
     .function = test_invalid_integer_pictures_are_rejected},
    {.name = "decimal_formats_apply_and_check_properties",
     .function = test_decimal_formats_apply_and_check_properties},
    {.name = "number_pictures_format", .function = test_number_pictures_format},
    {.name = "invalid_number_pictures_are_rejected",
     .function = test_invalid_number_pictures_are_rejected},
    {.name = "engine_survives_every_allocation_failure",
     .function = test_engine_survives_every_allocation_failure},
    {.name = "format_integer_through_the_api", .function = test_format_integer_through_the_api},
    {.name = "format_number_through_the_api", .function = test_format_number_through_the_api},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "format",
            .cases = format_test_cases,
            .case_count = SXML_TEST_COUNT(format_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
