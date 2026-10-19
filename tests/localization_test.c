/*
 * Tests for the localization feature (src/language_table.c,
 * src/spell_out.c, src/cldr.c, src/calendar.c; packet 7.17, D221)
 * through the XPath API: a processor that names
 * sxml_localization_feature() and serves the vendored CLDR files
 * (third_party/cldr/common) formats dates in German, French and Italian,
 * in the calendars BE, JE, OS and AH, and numbers in words and ordinals by
 * CLDR's spell-out rules; without the feature, or for a language without
 * files, the fallback markers and English stay. Every fixture checks for
 * leaks; an allocation-failure loop covers building a language table.
 *
 * Golden file: tests/timezone_test.c (a feature over a resolver of
 * vendored files, cases through the XPath API).
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

enum { SXML_TEST_PATH_BYTE_COUNT = 4096 };
enum { SXML_TEST_MAXIMUM_FILE_BYTE_COUNT = 1 << 21 };

/* ------------------------------------------------------------ Resolver */

/* Serves urn:sxml:locale-data:PATH from a CLDR common/ directory, or
   nothing when directory is NULL. */
typedef struct cldr_resolver {
    const char *directory;
} cldr_resolver;

static sxml_status resolve_cldr(void *state, const sxml_resource_request *request,
                                sxml_resource *resource) {
    const cldr_resolver *resolver = state;
    const sxml_byte_view prefix = TEXT("urn:sxml:locale-data:");
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    const sxml_byte_view uri = request->absolute_uri;
    if (request->resource_kind != SXML_RESOURCE_KIND_LOCALE_DATA || resolver->directory == NULL ||
        uri.byte_count <= prefix.byte_count || uri.byte_count > 300 ||
        memcmp(uri.bytes, prefix.bytes, prefix.byte_count) != 0) {
        return SXML_STATUS_OK;
    }
    char path[SXML_TEST_PATH_BYTE_COUNT];
    (void)snprintf(path, sizeof path, "%s%.*s", resolver->directory,
                   (int)(uri.byte_count - prefix.byte_count),
                   (const char *)uri.bytes + prefix.byte_count);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        sxml_default_allocator(),
        (sxml_allocation_request){.byte_count = SXML_TEST_MAXIMUM_FILE_BYTE_COUNT, .alignment = 1},
        &memory);
    if (status == SXML_STATUS_OK) {
        const size_t byte_count = fread(memory, 1, SXML_TEST_MAXIMUM_FILE_BYTE_COUNT, file);
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = (sxml_byte_view){.bytes = memory, .byte_count = byte_count};
    }
    (void)fclose(file);
    return status;
}

static void release_cldr(void *state, const sxml_resource *resource) {
    (void)state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(sxml_default_allocator(), (void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations cldr_resolver_operations = {
    .resolve = resolve_cldr,
    .release = release_cldr,
};

/* The vendored CLDR common/ directory with a trailing "/", in path; false
   when it cannot be read. */
static bool find_vendored_cldr(char path[SXML_TEST_PATH_BYTE_COUNT]) {
    const char *source_file = __FILE__;
    const char *last_separator = strrchr(source_file, '/');
    const size_t directory_byte_count =
        last_separator == NULL ? 0 : (size_t)(last_separator - source_file) + 1;
    (void)snprintf(path, SXML_TEST_PATH_BYTE_COUNT, "%.*s../third_party/cldr/common/",
                   (int)directory_byte_count, source_file);
    char probe[SXML_TEST_PATH_BYTE_COUNT + 16];
    (void)snprintf(probe, sizeof probe, "%smain/de.xml", path);
    FILE *file = fopen(probe, "rb");
    if (file == NULL) {
        printf("skipped: the vendored CLDR files are not reachable at %s\n", probe);
        return false;
    }
    (void)fclose(file);
    return true;
}

/* ------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    cldr_resolver resolver;
} fixture;

typedef struct fixture_setup {
    uint64_t failing_allocation_number;
    bool names_feature;
    const char *cldr_directory;
} fixture_setup;

static sxml_status fixture_create(fixture_setup setup, fixture *test_fixture) {
    memset(test_fixture, 0, sizeof(*test_fixture));
    test_fixture->resolver.directory = setup.cldr_directory;
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = setup.failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
    const sxml_optional_feature feature = sxml_localization_feature();
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = {.state = &test_fixture->resolver,
                                  .operations = &cldr_resolver_operations},
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = setup.names_feature ? 1 : 0}},
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

typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_error_record error_record;
} query_run;

/* Compiles and evaluates the expression and writes its string value into
   the fixture's output buffer (items separated by " "). */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        sxml_counting_allocator_as_allocator(test_fixture->counting_allocator),
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
            .serialization_parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_TEXT,
                                         .has_item_separator = true,
                                         .item_separator = TEXT(" ")},
            .error_record = &run->error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    sxml_sequence_release(result);
    return status;
}

typedef struct expression_case {
    const char *expression_text;
    const char *expected_output;
} expression_case;

static bool case_holds(const fixture *test_fixture, const expression_case *test_case) {
    query_run run = {.expression_text = sxml_byte_view_from_c_string(test_case->expression_text)};
    const sxml_status status = run_query(test_fixture, &run);
    const bool holds =
        status == SXML_STATUS_OK &&
        sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                              sxml_byte_view_from_c_string(test_case->expected_output));
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

typedef struct case_list {
    const expression_case *cases;
    size_t case_count;
} case_list;

#define CASE_LIST(array) ((case_list){.cases = (array), .case_count = SXML_TEST_COUNT(array)})

static void expect_all(sxml_test_state *state, fixture_setup setup, case_list list) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(setup, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        SXML_TEST_EXPECT(state, case_holds(&test_fixture, &list.cases[case_index]));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------------- Dates */

#define SOFT_HYPHEN "\xC2\xAD"

static void test_dates_have_the_names_of_languages(sxml_test_state *state) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    if (!find_vendored_cldr(directory)) {
        return;
    }
    static const expression_case cases[] = {
        /* QT4 format-date-de101, -de103, -de104, -de114, -de116. */
        {"string-join(for $m in 1 to 12 return format-date(xs:date('2003-01-07') + "
         "xs:yearMonthDuration('P1M') * ($m - 1), '[MN]', 'de', (), ()), ' ')",
         "JANUAR FEBRUAR M\xC3\x84RZ APRIL MAI JUNI JULI AUGUST SEPTEMBER OKTOBER NOVEMBER "
         "DEZEMBER"},
        {"format-date(xs:date('2003-03-07'), '[MNn]', 'de', (), ())", "M\xC3\xA4rz"},
        {"string-join(for $m in (1, 3, 9) return format-date(xs:date('2003-01-07') + "
         "xs:yearMonthDuration('P1M') * ($m - 1), '[MN,3-3]', 'de', (), ()), ' ')",
         "JAN M\xC3\x84R SEP"},
        {"format-date(xs:date('2003-12-08'), '[FN,2-2]', 'de', (), ())", "MO"},
        {"format-date(xs:date('2003-12-07'), '[FNn,2-2]', 'de', (), ())", "So"},
        /* A region's file before its language's (de_AT, then de). */
        {"format-date(xs:date('2003-01-07'), '[MNn]', 'de-AT')", "J\xC3\xA4nner"},
        {"format-date(xs:date('2003-02-07'), '[MNn]', 'de-AT')", "Februar"},
        {"format-date(xs:date('2003-12-07'), '[FNn], [D1o] [MNn] [Y]', 'fr')",
         "Dimanche, 7e D\xC3\xA9" "cembre 2003"},
        {"format-date(xs:date('2003-12-07'), '[Y] [E]', 'de')", "2003 n. Chr."},
        {"format-date(xs:date('-0054-03-15'), '[Y] [EN]', 'de')", "55 V. CHR."},
        {"format-dateTime(xs:dateTime('2003-12-07T15:00:00'), '[h] [PN]', 'de')", "3 PM"},
        /* The day in German words and ordinals. */
        {"format-date(xs:date('2003-12-07'), '[Dw] [Dwo]', 'de')", "sieben siebte"},
        /* A language without files, and the default language's own. */
        {"format-date(xs:date('2003-03-07'), '[MNn]', 'xib')", "[Language: en]March"},
        {"format-date(xs:date('2003-03-07'), '[MNn]', 'en')", "March"},
        {"format-date(xs:date('2003-03-07'), '[MNn]', 'de', 'CB', ())",
         "[Calendar: AD]M\xC3\xA4rz"},
    };
    expect_all(state, (fixture_setup){.names_feature = true, .cldr_directory = directory},
               CASE_LIST(cases));
}

static void test_dates_have_other_calendars(sxml_test_state *state) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    if (!find_vendored_cldr(directory)) {
        return;
    }
    static const expression_case cases[] = {
        /* Buddhist Era: 543 years before AD. */
        {"format-date(xs:date('2003-12-07'), '[D] [MNn] [Y] [E]', 'en', 'BE')",
         "7 December 2546 BE"},
        /* Japanese eras by their start dates; before them the Gregorian
           eras. */
        {"format-date(xs:date('2019-05-01'), '[E] [Y]', 'en', 'JE')", "Reiwa 1"},
        {"format-date(xs:date('2019-04-30'), '[E] [Y]', 'en', 'JE')", "Heisei 31"},
        {"format-date(xs:date('1989-01-07'), '[E] [Y]', 'en', 'JE')", "Sh\xC5\x8Dwa 64"},
        {"format-date(xs:date('0600-01-01'), '[Y] [E]', 'en', 'JE')", "600 AD"},
        /* Old Style: the Julian calendar is 13 days behind in 2024. */
        {"format-date(xs:date('2024-01-14'), '[D] [MNn] [Y] [E]', 'en', 'OS')",
         "1 January 2024 AD"},
        {"format-date(xs:date('2024-01-13'), '[D] [MNn] [Y] [d]', 'en', 'OS')",
         "31 December 2023 365"},
        {"format-date(xs:date('1582-10-15'), '[D] [MNn] [Y] [FNn]', 'en', 'OS')",
         "5 October 1582 Friday"},
        /* Anno Hegirae, tabular: 1 Muharram 1 AH is 16 July 622 (Julian). */
        {"format-date(xs:date('0622-07-19'), '[D] [MNn] [Y] [E]', 'en', 'AH')",
         "1 Muharram 1 AH"},
        {"format-date(xs:date('2024-07-08'), '[D] [MNn] [Y]', 'en', 'AH')", "1 Muharram 1446"},
        {"format-date(xs:date('2024-07-07'), '[D] [MNn] [Y] [d]', 'en', 'AH')",
         "30 Dhu\xCA\xBB" "l-Hijjah 1445 355"},
        /* German names in another calendar. */
        {"format-date(xs:date('2003-12-07'), '[MNn] [Y]', 'de', 'BE')", "Dezember 2546"},
        {"format-date(xs:date('2003-12-07'), '[C]', 'en', 'BE')", "BE"},
    };
    expect_all(state, (fixture_setup){.names_feature = true, .cldr_directory = directory},
               CASE_LIST(cases));
}

static void test_fallback_markers_stay_without_data(sxml_test_state *state) {
    static const expression_case cases[] = {
        {"format-date(xs:date('2003-03-07'), '[MNn]', 'de')", "[Language: en]March"},
        {"format-date(xs:date('2003-12-07'), '[Y]', 'en', 'BE')", "[Calendar: AD]2003"},
        {"format-date(xs:date('2003-12-07'), '[Y]', 'de', 'JE')",
         "[Language: en][Calendar: AD]2003"},
        {"format-integer(3, 'w', 'de')", "three"},
        {"format-integer(21, '1;o', 'fr')", "21st"},
    };
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    expect_all(state, (fixture_setup){.names_feature = false, .cldr_directory = NULL},
               CASE_LIST(cases));
    if (find_vendored_cldr(directory)) {
        /* The feature without data. */
        expect_all(state, (fixture_setup){.names_feature = true, .cldr_directory = NULL},
                   CASE_LIST(cases));
    }
}

/* ---------------------------------------------------------- Numbers */

static void test_numbers_have_spell_out_rules(sxml_test_state *state) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    if (!find_vendored_cldr(directory)) {
        return;
    }
    static const expression_case cases[] = {
        /* QT4 format-integer-032, -032-fr, -032-it, -065, -066, -076,
           -077. */
        {"format-integer(1, 'Ww;o(-er)', 'de')", "Erster"},
        {"format-integer(2, 'Ww;o', 'fr')", "Deuxi\xC3\xA8me"},
        {"format-integer(5, 'Ww;o(-o)', 'it') || ';' || format-integer(5, 'Ww;o(-a)', 'it')",
         "Quinto;Quinta"},
        {"format-integer(20, 'Ww;o(%spellout-ordinal)', 'de')", "Zwanzigste"},
        {"format-integer(5, 'Ww;o(%spellout-ordinal-masculine)', 'it')", "Quinto"},
        {"format-integer(20, 'Ww;c(%spellout-cardinal)', 'de')", "Zwanzig"},
        {"string-join((format-integer(1, 'Ww;c(%spellout-ordinal-s)', 'de'), "
         "format-integer(1, 'Ww;c(%spellout-ordinal-r)', 'de'), "
         "format-integer(1, 'Ww;c(%spellout-ordinal-n)', 'de')), ' ')",
         "Erstes Erster Ersten"},
        /* Cardinals: brackets, rollback, case, ß in upper case. */
        {"format-integer(134, 'W', 'de')",
         "EIN" SOFT_HYPHEN "HUNDERT" SOFT_HYPHEN "VIER" SOFT_HYPHEN "UND" SOFT_HYPHEN
         "DREISSIG"},
        {"format-integer(200, 'w', 'de')", "zwei" SOFT_HYPHEN "hundert"},
        {"format-integer(2134816, 'Ww', 'de')",
         "Zwei Millionen Ein" SOFT_HYPHEN "hundert" SOFT_HYPHEN "vier" SOFT_HYPHEN "und" SOFT_HYPHEN
         "drei\xC3\x9Fig" SOFT_HYPHEN "tausend" SOFT_HYPHEN "acht" SOFT_HYPHEN "hundert" SOFT_HYPHEN
         "sechzehn"},
        {"format-integer(71, 'w', 'fr')", "soixante-et-onze"},
        {"format-integer(-3, 'w', 'nl')", "-drie"},
        /* English by its rules only when a rule set is named. */
        {"format-integer(120, 'w;o(%spellout-ordinal)', 'en')", "one hundred twentieth"},
        {"format-integer(120, 'w;o(%spellout-ordinal-verbose)', 'en')",
         "one hundred and twentieth"},
        {"format-integer(120, 'w', 'en')", "one hundred and twenty"},
        /* Ordinal suffixes of digits, by the ordinal plural rules. */
        {"format-integer(1, '1;o', 'fr') || ' ' || format-integer(2, '1;o', 'fr')", "1er 2e"},
        {"format-integer(1234, '#,##0;o', 'es')", "1,234.\xC2\xBA"},
        {"format-integer(1234, '#,##0;o', 'de')", "1,234"},
        /* Too large for the rules: English words. */
        {"format-integer(123456789012345678901, 'w', 'de')",
         "one hundred and twenty-three quintillion four hundred and fifty-six quadrillion seven "
         "hundred and eighty-nine trillion twelve billion three hundred and forty-five million "
         "six hundred and seventy-eight thousand nine hundred and one"},
    };
    expect_all(state, (fixture_setup){.names_feature = true, .cldr_directory = directory},
               CASE_LIST(cases));
}

/* ------------------------------------------------------ Small data */

/* A language "xx" of a few CLDR-shaped files: inheritance from xx_YY to
   xx to root, the inheritance marker, aliases within a calendar and
   between calendars, dated eras, spell-out rules with brackets and plural
   choices. */
static const char small_root[] =
    "<ldml><dates><calendars>"
    "<calendar type='gregorian'><months><monthContext type='format'>"
    "<monthWidth type='abbreviated'><alias source='locale' path=\"../monthWidth[@type='wide']\"/>"
    "</monthWidth><monthWidth type='wide'>"
    "<month type='1'>M01</month><month type='2'>M02</month><month type='3'>M03</month>"
    "<month type='4'>M04</month><month type='5'>M05</month><month type='6'>M06</month>"
    "<month type='7'>M07</month><month type='8'>M08</month><month type='9'>M09</month>"
    "<month type='10'>M10</month><month type='11'>M11</month><month type='12'>M12</month>"
    "</monthWidth></monthContext></months>"
    "<days><dayContext type='format'><dayWidth type='wide'>"
    "<day type='sun'>D7</day><day type='mon'>D1</day><day type='tue'>D2</day>"
    "<day type='wed'>D3</day><day type='thu'>D4</day><day type='fri'>D5</day>"
    "<day type='sat'>D6</day></dayWidth></dayContext></days>"
    "<eras><eraAbbr><era type='0'>BCE</era><era type='1'>CE</era></eraAbbr></eras>"
    "</calendar>"
    "<calendar type='japanese'><months><alias source='locale' "
    "path=\"../../calendar[@type='gregorian']/months\"/></months>"
    "<days><alias source='locale' path=\"../../calendar[@type='gregorian']/days\"/></days>"
    "<eras><eraAbbr><era type='0'>Taika</era><era type='1'>Heisei</era></eraAbbr></eras>"
    "</calendar></calendars></dates></ldml>";

static const char small_language[] =
    "<?xml version='1.0'?><ldml><dates><calendars><calendar type='gregorian'>"
    "<months><monthContext type='format'><monthWidth type='wide'>"
    "<month type='1'>Primo</month><month type='2' draft='contributed'>\xE2\x86\x91\xE2\x86\x91"
    "\xE2\x86\x91</month></monthWidth></monthContext></months>"
    "<eras><eraAbbr><era type='1'>AX</era></eraAbbr></eras>"
    "</calendar></calendars></dates></ldml>";

static const char small_region[] =
    "<ldml><dates><calendars><calendar type='gregorian'>"
    "<months><monthContext type='format'><monthWidth type='wide'>"
    "<month type='1'>Premier</month></monthWidth></monthContext></months>"
    "</calendar></calendars></dates></ldml>";

static const char small_supplemental[] =
    "<supplementalData><calendarData>"
    "<calendar type='gregorian'><eras><era type='0' end='0-12-31'/>"
    "<era type='1' start='1-01-01'/></eras></calendar>"
    "<calendar type='japanese'><inheritEras calendar='gregorian'/><eras>"
    "<era type='0' start='645-6-19'/><era type='1' start='1989-1-8'/></eras></calendar>"
    "</calendarData></supplementalData>";

static const char small_rules[] =
    "<ldml><rbnf><rulesetGrouping type='SpelloutRules'>"
    "<ruleset type='spellout-numbering'>"
    "<rbnfrule value='-x'>minus \xE2\x86\x92\xE2\x86\x92;</rbnfrule>"
    "<rbnfrule value='0'>zero;</rbnfrule><rbnfrule value='1'>one;</rbnfrule>"
    "<rbnfrule value='2'>two;</rbnfrule><rbnfrule value='3'>three;</rbnfrule>"
    "<rbnfrule value='4'>four;</rbnfrule><rbnfrule value='5'>five;</rbnfrule>"
    "<rbnfrule value='6'>six;</rbnfrule><rbnfrule value='7'>seven;</rbnfrule>"
    "<rbnfrule value='8'>eight;</rbnfrule><rbnfrule value='9'>nine;</rbnfrule>"
    "<rbnfrule value='10'>ten;</rbnfrule>"
    "<rbnfrule value='20'>twenty[-\xE2\x86\x92\xE2\x86\x92];</rbnfrule>"
    "<rbnfrule value='100'>\xE2\x86\x90\xE2\x86\x90-hundred[ \xE2\x86\x92\xE2\x86\x92];</rbnfrule>"
    "</ruleset>"
    "<ruleset type='spellout-ordinal'>"
    "<rbnfrule value='0'>=%spellout-numbering=th;</rbnfrule>"
    "</ruleset>"
    "<ruleset type='loop' access='private'>"
    "<rbnfrule value='0'>=%%loop=;</rbnfrule>"
    "</ruleset>"
    "<ruleset type='spellout-cardinal-loop'>"
    "<rbnfrule value='0'>x\xE2\x86\x92%%loop\xE2\x86\x92;</rbnfrule>"
    "</ruleset>"
    "</rulesetGrouping><rulesetGrouping type='OrdinalRules'>"
    "<ruleset type='digits-ordinal'>"
    "<rbnfrule value='0'>=#,##0=$(ordinal,one{st}two{nd}other{th})$;</rbnfrule>"
    "</ruleset></rulesetGrouping></rbnf></ldml>";

static const char small_ordinals[] =
    "<supplementalData><plurals type='ordinal'>"
    "<pluralRules locales='xx yy'>"
    "<pluralRule count='one'>n % 10 = 1 and n % 100 != 11 @integer 1, 21</pluralRule>"
    "<pluralRule count='two'>n % 10 = 2 and n % 100 != 12 or n = 1000..1002 @integer 2</pluralRule>"
    "<pluralRule count='other'> @integer 0, 3~17</pluralRule>"
    "</pluralRules></plurals></supplementalData>";

#define SMALL_ENTRY(path, text)                                                            \
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:sxml:locale-data:" path),                       \
     .content = {.bytes = (const uint8_t *)(text), .byte_count = sizeof(text) - 1}}

static const sxml_resource_entry small_entries[] = {
    SMALL_ENTRY("main/root.xml", small_root),
    SMALL_ENTRY("main/xx.xml", small_language),
    SMALL_ENTRY("main/xx_YY.xml", small_region),
    SMALL_ENTRY("supplemental/supplementalData.xml", small_supplemental),
    SMALL_ENTRY("rbnf/xx.xml", small_rules),
    SMALL_ENTRY("supplemental/ordinals.xml", small_ordinals),
};

static const sxml_resource_entry_view small_entry_view = {
    .resource_entries = small_entries, .resource_entry_count = SXML_TEST_COUNT(small_entries)};

static const expression_case small_cases[] = {
    {"format-date(xs:date('2003-01-07'), '[MNn] [MNn,3-3]', 'xx')", "Primo Pri"},
    {"format-date(xs:date('2003-02-07'), '[MNn]', 'xx')", "M02"},
    {"format-date(xs:date('2003-01-07'), '[MNn] [Y] [E]', 'xx-YY')", "Premier 2003 AX"},
    {"format-date(xs:date('-0001-01-07'), '[Y] [E] [FNn]', 'xx')", "2 BCE D4"},
    {"format-date(xs:date('2019-05-01'), '[E] [Y] [MNn] [FNn]', 'xx', 'JE')", "Heisei 31 M05 D3"},
    {"format-date(xs:date('0700-05-01'), '[E] [Y]', 'xx', 'JE')", "Taika 56"},
    {"format-date(xs:date('0600-05-01'), '[E] [Y]', 'xx', 'JE')", "AX 600"},
    {"format-integer(123, 'w', 'xx')", "one-hundred twenty-three"},
    {"format-integer(200, 'Ww', 'xx')", "Two-hundred"},
    {"format-integer(20, 'W;o', 'xx')", "TWENTYTH"},
    {"string-join(for $n in (1, 2, 3, 11, 21, 1000, 1001) return format-integer($n, '1;o', "
     "'xx'), ' ')",
     "1st 2nd 3th 11th 21st 1000nd 1001st"},
    {"format-integer(3, 'w;c(%spellout-cardinal-loop)', 'xx')", "three"},
    {"format-date(xs:date('2003-02-07'), '[MNn]', 'yy')", "[Language: en]February"},
};

static sxml_status small_fixture_create(uint64_t failing_number, fixture *test_fixture) {
    memset(test_fixture, 0, sizeof(*test_fixture));
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number = failing_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
    const sxml_optional_feature feature = sxml_localization_feature();
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = sxml_table_resource_resolver(&small_entry_view),
            .optional_features = {.optional_features = &feature, .optional_feature_count = 1}},
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

static void test_small_data_follows_inheritance_and_aliases(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, small_fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(small_cases); case_index += 1) {
        SXML_TEST_EXPECT(state, case_holds(&test_fixture, &small_cases[case_index]));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Names, eras and words of one table. */
static const expression_case small_failure_cases[] = {
    {"format-date(xs:date('2019-05-01'), '[E] [Y] [MNn] [FNn] [Dwo]', 'xx-YY', 'JE')",
     "Heisei 31 M05 D3 oneth"},
    {"format-integer(21, '1;o', 'xx')", "21st"},
};

/* Building language tables fails cleanly at every allocation (D23). */
static void test_tables_survive_every_allocation_failure(sxml_test_state *state) {
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; !has_succeeded && failing_number < 100000;
         failing_number += 1) {
        fixture test_fixture;
        bool is_whole = small_fixture_create(failing_number, &test_fixture) == SXML_STATUS_OK;
        for (size_t case_index = 0;
             is_whole && case_index < SXML_TEST_COUNT(small_failure_cases); case_index += 1) {
            const expression_case *test_case = &small_failure_cases[case_index];
            query_run run = {.expression_text =
                                 sxml_byte_view_from_c_string(test_case->expression_text)};
            const sxml_status status = run_query(&test_fixture, &run);
            is_whole = status == SXML_STATUS_OK &&
                       sxml_byte_view_equals(
                           sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                           sxml_byte_view_from_c_string(test_case->expected_output));
            SXML_TEST_EXPECT(state, is_whole || status == SXML_STATUS_OUT_OF_MEMORY ||
                                        status == SXML_STATUS_DYNAMIC_ERROR);
        }
        has_succeeded = is_whole;
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case localization_test_cases[] = {
    {.name = "dates_have_the_names_of_languages",
     .function = test_dates_have_the_names_of_languages},
    {.name = "dates_have_other_calendars", .function = test_dates_have_other_calendars},
    {.name = "fallback_markers_stay_without_data",
     .function = test_fallback_markers_stay_without_data},
    {.name = "numbers_have_spell_out_rules", .function = test_numbers_have_spell_out_rules},
    {.name = "small_data_follows_inheritance_and_aliases",
     .function = test_small_data_follows_inheritance_and_aliases},
    {.name = "tables_survive_every_allocation_failure",
     .function = test_tables_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "localization",
            .cases = localization_test_cases,
            .case_count = SXML_TEST_COUNT(localization_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
