/*
 * Tests for the Snowball stemmers of Full Text (src/full_text_internal.h,
 * "Stemming"; packet 7.12c, D214): words of each language's vocabulary in
 * snowball-data (github.com/snowballstem/snowball-data, the files voc.txt
 * and output.txt), with the stems Snowball 3.1.1 gives them. The packet
 * checked every word of the six vocabularies (210,000) the same way; these
 * are a sample. Also: words too long to stem, the language tags, and that
 * the machine leaks nothing.
 *
 * Golden file: tests/regex_test.c (an internal engine over a table of
 * inputs and expected outputs).
 */
#include "full_text_internal.h"

#include "sxml/memory.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* A word and its stem. */
typedef struct stem_case {
    sxml_full_text_language language;
    const char *word;
    const char *stem;
} stem_case;

static const stem_case stem_cases[] = {
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "archly", "arch"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "attrapped", "attrap"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "bar", "bar"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "barge", "barg"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "bethrothed", "bethroth"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "binnacle", "binnacl"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "brawny", "brawni"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "browsed", "brows"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "buries", "buri"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "consistent", "consist"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "disillusioned", "disillus"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "ejaculate", "ejacul"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "glances", "glanc"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "idleness", "idl"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "inheritor", "inheritor"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "insuppressive", "insuppress"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "kolomensky", "kolomenski"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "liquors", "liquor"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "outcry", "outcri"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "petulant", "petul"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "pools", "pool"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "quatre", "quatr"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "separates", "separ"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "tertian", "tertian"},
    {SXML_FULL_TEXT_LANGUAGE_ENGLISH, "writhes", "writh"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "aufkrochen", "aufkroch"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "aufsproßt", "aufsprosst"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "aussetzen", "aussetz"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "botschafters", "botschaft"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "bundespräsident", "bundesprasident"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "darbietende", "darbiet"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "donnernd", "donnernd"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "dreihundertfünfzig", "dreihundertfunfz"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "flechte", "flecht"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "fluktuationsrate", "fluktuationsrat"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "genius", "genius"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "gesindel", "gesindel"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "glashelle", "glashell"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "gültigkeit", "gultig"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "kräftige", "kraftig"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "leute", "leut"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "schleiften", "schleift"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "schwindler", "schwindl"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "selbstprüfung", "selbstpruf"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "silbernem", "silbern"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "sorgfältigsten", "sorgfalt"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "sprüche", "spruch"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "stammschlosses", "stammschloss"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "tücke", "tuck"},
    {SXML_FULL_TEXT_LANGUAGE_GERMAN, "umhalsten", "umhal"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "boniface", "bonifac"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "boutade", "boutad"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "chargera", "charg"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "compatriotes", "compatriot"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "emparant", "empar"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "equinoctial", "equinoctial"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "fantaisie", "fantais"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "huile", "huil"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "ignares", "ignar"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "lisses", "liss"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "liverpool", "liverpool"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "livrais", "livr"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "mathilde", "mathild"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "obéisse", "obéiss"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "passées", "pass"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "piété", "piet"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "renversa", "renvers"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "révélé", "rével"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "sentisses", "sent"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "solennel", "solennel"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "tenir", "ten"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "territoire", "territoir"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "vocation", "vocat"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "ébouriffé", "ébouriff"},
    {SXML_FULL_TEXT_LANGUAGE_FRENCH, "étonnée", "éton"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "asciugata", "asciug"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "biancastri", "biancastr"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "borsistico", "borsist"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "burgo", "burg"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "camperebbe", "camp"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "carla", "carl"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "colombo", "colomb"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "corrisposta", "corrispost"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "disgustosa", "disgust"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "elisabetta", "elisabett"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "magona", "magon"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "me", "me"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "originariamente", "originar"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "paglia", "pagl"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "predisposto", "predispost"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "prendervi", "prend"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "prolungar", "prolung"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "rimmel", "rimmel"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "scusandosi", "scus"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "semicieco", "semiciec"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "strettala", "strettal"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "tabarez", "tabarez"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "tolone", "tolon"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "undecimo", "undecim"},
    {SXML_FULL_TEXT_LANGUAGE_ITALIAN, "venezuela", "venezuel"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "afecte", "afect"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "aranceles", "arancel"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "avalan", "aval"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "capacidades", "capac"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "celebrará", "celebr"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "conducido", "conduc"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "confidente", "confident"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "deidad", "deid"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "dictamen", "dictam"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "engorrosas", "engorr"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "fresca", "fresc"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "gaspar", "gasp"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "genetec", "genetec"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "impondrán", "impondran"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "incorporados", "incorpor"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "llegara", "lleg"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "managers", "managers"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "mandaron", "mand"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "muelles", "muell"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "mutilados", "mutil"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "rangos", "rang"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "raphael", "raphael"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "satanás", "satanas"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "troncales", "troncal"},
    {SXML_FULL_TEXT_LANGUAGE_SPANISH, "vota", "vot"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "aanzetstuk", "aanzetstuk"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "aardgasbesparing", "aardgasbespaar"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "bewaakt", "bewaak"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "deltawig", "deltaw"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "doorpriemde", "doorpriem"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "doorspraken", "doorspraak"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "eep", "eep"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "energiefonds", "energiefond"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "ettlingen", "ettl"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "gemodificeerd", "modificeer"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "generaalskostuum", "neraalskostuum"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "genomineerden", "nomineer"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "glimlachten", "glimlacht"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "handelsbeurs", "handelsbeur"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "kunststofrecycling", "kunststofrecycl"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "levert", "levert"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "loodjes", "lood"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "mineralenvermindering", "mineralenverminder"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "nazuiveringsruimte", "nazuiveringsruimt"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "opneembare", "opneembar"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "rhythmische", "rhythmisch"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "softstarters", "softstarter"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "uitputtend", "uitput"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "vergunningenstelsel", "vergunninnstel"},
    {SXML_FULL_TEXT_LANGUAGE_DUTCH, "verwering", "verweer"},
};

/* Stems word in language into *output; returns its status. */
typedef struct stemming {
    sxml_full_text_language language;
    sxml_byte_view word;
    sxml_full_text_stem_workspace *workspace;
} stemming;

static sxml_status stem_word(stemming request, sxml_full_text_bytes *output) {
    output->byte_count = 0;
    return sxml_full_text_stem(&(sxml_full_text_stem_request){.word = request.word,
                                                               .language = request.language,
                                                               .workspace = request.workspace},
                               output);
}

static bool output_is(const sxml_full_text_bytes *output, const char *expected) {
    return sxml_byte_view_equals((sxml_byte_view){.bytes = output->bytes,
                                                  .byte_count = output->byte_count},
                                 (sxml_byte_view){.bytes = (const uint8_t *)expected,
                                                  .byte_count = strlen(expected)});
}

static void test_words_stem_as_snowball_stems_them(sxml_test_state *state) {
    sxml_counting_allocator *counting = NULL;
    SXML_TEST_REQUIRE(state, sxml_counting_allocator_create(
                                sxml_default_allocator(),
                                &(sxml_counting_allocator_configuration){
                                    .backing_allocator = sxml_default_allocator(),
                                    .failing_allocation_number = 0},
                                &counting) == SXML_STATUS_OK);
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting);
    sxml_full_text_stem_workspace workspace = {.allocator = allocator};
    sxml_full_text_bytes output = {.allocator = allocator};
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(stem_cases); case_index += 1) {
        const stem_case *entry = &stem_cases[case_index];
        const sxml_status status = stem_word(
            (stemming){.language = entry->language,
                       .word = {.bytes = (const uint8_t *)entry->word,
                                .byte_count = strlen(entry->word)},
                       .workspace = &workspace},
            &output);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && output_is(&output, entry->stem));
        if (status != SXML_STATUS_OK || !output_is(&output, entry->stem)) {
            (void)fprintf(stderr, "  %s: got %.*s\n", entry->word, (int)output.byte_count,
                          (const char *)output.bytes);
        }
    }
    /* A word beyond SXML_FULL_TEXT_MAXIMUM_STEMMED_LENGTH stays as it is. */
    uint8_t long_word[300];
    memset(long_word, 'a', sizeof long_word);
    long_word[sizeof long_word - 1] = 's';
    SXML_TEST_EXPECT(state,
                     stem_word((stemming){.language = SXML_FULL_TEXT_LANGUAGE_ENGLISH,
                                          .word = {.bytes = long_word,
                                                   .byte_count = sizeof long_word},
                                          .workspace = &workspace},
                               &output) == SXML_STATUS_OK &&
                         output.byte_count == sizeof long_word);
    SXML_TEST_EXPECT(state, stem_word((stemming){.language = SXML_FULL_TEXT_LANGUAGE_ENGLISH,
                                                 .word = TEXT(""),
                                                 .workspace = &workspace},
                                      &output) == SXML_STATUS_OK &&
                                output.byte_count == 0);
    sxml_full_text_bytes_release(&output);
    sxml_full_text_stem_workspace_release(&workspace);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(counting).live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting);
}

static void test_language_tags_choose_stemmers(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_full_text_language_of(TEXT("de")) == SXML_FULL_TEXT_LANGUAGE_GERMAN);
    SXML_TEST_EXPECT(state,
                     sxml_full_text_language_of(TEXT("FR-ca")) == SXML_FULL_TEXT_LANGUAGE_FRENCH);
    SXML_TEST_EXPECT(state, sxml_full_text_language_of(TEXT("nl")) == SXML_FULL_TEXT_LANGUAGE_DUTCH);
    SXML_TEST_EXPECT(state, sxml_full_text_language_of(TEXT("")) == SXML_FULL_TEXT_LANGUAGE_ENGLISH);
    SXML_TEST_EXPECT(state,
                     sxml_full_text_language_of(TEXT("eng")) == SXML_FULL_TEXT_LANGUAGE_ENGLISH);
}

static const sxml_test_case full_text_stem_test_cases[] = {
    {.name = "words_stem_as_snowball_stems_them",
     .function = test_words_stem_as_snowball_stems_them},
    {.name = "language_tags_choose_stemmers", .function = test_language_tags_choose_stemmers},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "full_text_stem",
            .cases = full_text_stem_test_cases,
            .case_count = SXML_TEST_COUNT(full_text_stem_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
