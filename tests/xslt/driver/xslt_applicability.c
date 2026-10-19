/*
 * Applicability of XSLT 4.0 test cases (xslt_applicability.h). Imitates
 * tests/qt4/driver/applicability.c; dependency names and values follow
 * third_party/xslt40-test/admin/catalog-schema.xsd.
 */
#include "xslt_applicability.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

static bool sxml_xslt_test_starts_with(sxml_byte_view text,
                                       sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 ||
            memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

bool sxml_xslt_test_spec_token_matches(sxml_xslt_test_spec_token spec_token) {
    const sxml_byte_view prefix = SXML_TEXT("XSLT");
    const sxml_byte_view token = spec_token.token;
    if (!sxml_xslt_test_starts_with(token, prefix)) {
        return false;
    }
    size_t byte_index = prefix.byte_count;
    uint32_t version = 0;
    size_t digit_count = 0;
    while (byte_index < token.byte_count && token.bytes[byte_index] >= '0' &&
           token.bytes[byte_index] <= '9' && digit_count < 4) {
        version = version * 10 + (uint32_t)(token.bytes[byte_index] - '0');
        byte_index += 1;
        digit_count += 1;
    }
    if (digit_count != 2) {
        return false;
    }
    if (byte_index == token.byte_count) {
        return version == spec_token.version;
    }
    return byte_index + 1 == token.byte_count &&
           token.bytes[byte_index] == '+' && version <= spec_token.version;
}

/* ------------------------------------------------------------- The table */

/*
 * What sxml provides, as dependency type and whole value. A
 * schema-aware XSLT 4.0 processor (D179; no streaming, D97) with
 * serialization (packets 3.3 and 3.4) and higher-order functions. A
 * dependency type of the dependencyTypeTrue kind has no value; its entry
 * has an empty value. Grows with sxml.
 */
typedef struct sxml_xslt_test_supported_dependency {
    sxml_byte_view dependency_type;
    sxml_byte_view value;
} sxml_xslt_test_supported_dependency;

#define SXML_XSLT_TEST_SUPPORTED(type, value)                                  \
    {SXML_BYTE_VIEW_INITIALIZER(type), SXML_BYTE_VIEW_INITIALIZER(value)}

static const sxml_xslt_test_supported_dependency
    sxml_xslt_test_supported_dependencies[] = {
        SXML_XSLT_TEST_SUPPORTED("feature", "higher_order_functions"),
        SXML_XSLT_TEST_SUPPORTED("feature", "schema_aware"),
        SXML_XSLT_TEST_SUPPORTED("feature", "namespace_axis"),
        SXML_XSLT_TEST_SUPPORTED("feature", "serialization"),
        SXML_XSLT_TEST_SUPPORTED("feature", "built_in_derived_types"),
        /* The optional XPath 3.1 support of XSLT 3.0 (maps, arrays): XPath
           4.0 has it. */
        SXML_XSLT_TEST_SUPPORTED("feature", "XPath_3.1"),
        SXML_XSLT_TEST_SUPPORTED("feature", "HTML5"),
        /* XML 1.1 documents (D195): sources, stylesheets and assert-xml
           with xml-version="1.1" are read by XML 1.1. */
        SXML_XSLT_TEST_SUPPORTED("feature", "XML_1.1"),
        /* XSLT 4.0, 26.1.8, with the XML, XHTML and HTML methods (D188). */
        SXML_XSLT_TEST_SUPPORTED("feature", "disabling_output_escaping"),
        /* XSLT 1.0 behaviour (XSLT 4.0, 3.8.1; D_PACKET74). */
        SXML_XSLT_TEST_SUPPORTED("feature", "backwards_compatibility"),
        /* streamable="yes" runs unstreamed (D97). */
        SXML_XSLT_TEST_SUPPORTED("feature", "streaming-fallback"),
        SXML_XSLT_TEST_SUPPORTED("default_language_for_numbering", "en"),
        SXML_XSLT_TEST_SUPPORTED("languages_for_numbering", "en"),
        /* The alphabets and symbol sequences of src/format.c (packet
           4.17), each over the ranges its test cases name. */
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "CIRCLED DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "PARENTHESIZED DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "DIGIT ONE FULL STOP"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "DIGIT ONE COMMA"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "DINGBAT NEGATIVE CIRCLED DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "DOUBLE CIRCLED DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "DINGBAT CIRCLED SANS-SERIF DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "DINGBAT NEGATIVE CIRCLED SANS-SERIF DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "PARENTHESIZED IDEOGRAPH ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "CIRCLED IDEOGRAPH ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "AEGEAN NUMBER ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "COPTIC EPACT DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "RUMI DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "BRAHMI NUMBER ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "SINHALA ARCHAIC DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "COUNTING ROD UNIT DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "MENDE KIKAKUI DIGIT ONE"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "GREEK SMALL LETTER ALPHA (TRADITIONAL)"),
        SXML_XSLT_TEST_SUPPORTED("combinations_for_numbering",
                                 "GREEK SMALL LETTER ALPHA (ALPHABETIC)"),
        SXML_XSLT_TEST_SUPPORTED("supported_calendars_in_date_formatting_"
                                 "functions",
                                 "AD"),
        SXML_XSLT_TEST_SUPPORTED("supported_calendars_in_date_formatting_"
                                 "functions",
                                 "ISO"),
        SXML_XSLT_TEST_SUPPORTED("default_calendar_in_date_formatting_"
                                 "functions",
                                 "AD"),
        /* xs:date and friends of XSD 1.1, as the QT4 driver's year_lt_0
           and xsd-version 1.1 (D83). */
        SXML_XSLT_TEST_SUPPORTED("year_component_values",
                                 "support negative year"),
        SXML_XSLT_TEST_SUPPORTED("year_component_values",
                                 "support year above 9999"),
        SXML_XSLT_TEST_SUPPORTED("year_component_values", "support year zero"),
        /* Unicode normalization of Phase 2 (D107, D108). */
        SXML_XSLT_TEST_SUPPORTED("additional_normalization_form",
                                 "support NFD"),
        SXML_XSLT_TEST_SUPPORTED("additional_normalization_form",
                                 "support NFKC"),
        SXML_XSLT_TEST_SUPPORTED("additional_normalization_form",
                                 "support NFKD"),
        SXML_XSLT_TEST_SUPPORTED("additional_normalization_form",
                                 "support NFD NFKC NFKD"),
        SXML_XSLT_TEST_SUPPORTED("default_output_encoding", "UTF-8"),
        SXML_XSLT_TEST_SUPPORTED("unparsed_text_encoding", "UTF-8"),
        SXML_XSLT_TEST_SUPPORTED("default_html_version", "5"),
        /* XSLT 4.0, 6.6.1: the last matching rule is chosen unless the
           mode says on-multiple-match="fail". */
        SXML_XSLT_TEST_SUPPORTED("on-multiple-match", "recover"),
        /* Until packet 4.16 decides otherwise. */
        SXML_XSLT_TEST_SUPPORTED("package_version_resolution",
                                 "highest_version"),
        SXML_XSLT_TEST_SUPPORTED("enable_assertions", ""),
        SXML_XSLT_TEST_SUPPORTED("collation_uri",
                                 "http://www.w3.org/2005/xpath-functions/"
                                 "collation/codepoint"),
        SXML_XSLT_TEST_SUPPORTED("statically_known_collations",
                                 "http://www.w3.org/2005/xpath-functions/"
                                 "collation/codepoint"),
};

/* maximum_number_of_decimal_digits: xs:decimal has arbitrary precision
   in sxml (libmpdec, D74), so every count is supported. */
static bool sxml_xslt_test_is_decimal_digit_count(sxml_byte_view value) {
    if (value.byte_count == 0) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < value.byte_count;
         byte_index += 1) {
        if (value.bytes[byte_index] < '0' || value.bytes[byte_index] > '9') {
            return false;
        }
    }
    return true;
}

bool sxml_xslt_test_dependency_is_supported(
    sxml_qt4_dependency_value dependency) {
    if (sxml_byte_view_equals(dependency.dependency_type,
                              SXML_TEXT("maximum_number_of_decimal_digits"))) {
        return sxml_xslt_test_is_decimal_digit_count(dependency.value);
    }
    const size_t entry_count = sizeof sxml_xslt_test_supported_dependencies /
                               sizeof sxml_xslt_test_supported_dependencies[0];
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        const sxml_xslt_test_supported_dependency *entry =
            &sxml_xslt_test_supported_dependencies[entry_index];
        if (sxml_byte_view_equals(entry->dependency_type,
                                  dependency.dependency_type) &&
            (entry->value.byte_count == 0 ||
             sxml_byte_view_equals(entry->value, dependency.value))) {
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------- Scope */

static const sxml_byte_view sxml_xslt_test_non_goal_features[] = {
    SXML_BYTE_VIEW_INITIALIZER("schema_aware"),
    SXML_BYTE_VIEW_INITIALIZER("streaming"),
    SXML_BYTE_VIEW_INITIALIZER("dtd"),
    /* Supported since packet 7.13 (D195), but kept out of the D95
       denominator so that the gate rates keep their meaning. */
    SXML_BYTE_VIEW_INITIALIZER("XML_1.1"),
    SXML_BYTE_VIEW_INITIALIZER("XSD_1.1"),
    /* Supported since packet 7.4 (D_PACKET74), kept out of the D95
       denominator as XML_1.1 is. */
    SXML_BYTE_VIEW_INITIALIZER("backwards_compatibility"),
    SXML_BYTE_VIEW_INITIALIZER("HTML4"),
    SXML_BYTE_VIEW_INITIALIZER("ICU_spellout"),
};

bool sxml_xslt_test_feature_is_non_goal(sxml_byte_view value) {
    const size_t feature_count = sizeof sxml_xslt_test_non_goal_features /
                                 sizeof sxml_xslt_test_non_goal_features[0];
    for (size_t feature_index = 0; feature_index < feature_count;
         feature_index += 1) {
        if (sxml_byte_view_equals(
                value, sxml_xslt_test_non_goal_features[feature_index])) {
            return true;
        }
    }
    return false;
}

bool sxml_xslt_test_group_is_non_goal(sxml_byte_view group_name) {
    return sxml_byte_view_equals(group_name, SXML_TEXT("strm")) ||
           sxml_byte_view_equals(group_name, SXML_TEXT("sandp"));
}

/* -------------------------------------------------------------- Decision */

static bool sxml_xslt_test_is_spec(const sxml_qt4_dependency *dependency) {
    return sxml_byte_view_equals(dependency->dependency_type,
                                 SXML_TEXT("spec"));
}

static size_t
sxml_xslt_test_spec_count(sxml_qt4_dependency_view dependencies) {
    size_t spec_count = 0;
    for (size_t dependency_index = 0;
         dependency_index < dependencies.dependency_count;
         dependency_index += 1) {
        if (sxml_xslt_test_is_spec(&dependencies.dependencies[dependency_index])) {
            spec_count += 1;
        }
    }
    return spec_count;
}

/* True when one space-separated token of value admits XSLT 4.0. */
static bool sxml_xslt_test_spec_value_matches(sxml_byte_view value) {
    size_t byte_index = 0;
    while (byte_index < value.byte_count) {
        while (byte_index < value.byte_count && value.bytes[byte_index] == ' ') {
            byte_index += 1;
        }
        const size_t token_start = byte_index;
        while (byte_index < value.byte_count && value.bytes[byte_index] != ' ') {
            byte_index += 1;
        }
        if (byte_index > token_start &&
            sxml_xslt_test_spec_token_matches((sxml_xslt_test_spec_token){
                .token = {.bytes = value.bytes + token_start,
                          .byte_count = byte_index - token_start},
                .version = SXML_XSLT_TEST_TARGET_VERSION})) {
            return true;
        }
    }
    return false;
}

/* Writes the skip reason. */
typedef struct sxml_xslt_test_skip_reason {
    sxml_byte_view reason;
    sxml_byte_view detail;
    bool is_inverted;
} sxml_xslt_test_skip_reason;

static void sxml_xslt_test_skip(sxml_qt4_applicability *applicability,
                                sxml_xslt_test_skip_reason skip) {
    applicability->is_applicable = false;
    applicability->reason = skip.reason;
    applicability->detail = skip.detail;
    applicability->is_inverted = skip.is_inverted;
}

/* Rule 1. Every spec dependency of the deciding list must match. */
static bool
sxml_xslt_test_decide_spec(const sxml_xslt_test_case *test_case,
                           sxml_qt4_applicability *applicability) {
    const sxml_qt4_dependency_view deciding =
        sxml_xslt_test_spec_count(test_case->dependencies) != 0
            ? test_case->dependencies
            : test_case->test_set_dependencies;
    for (size_t dependency_index = 0;
         dependency_index < deciding.dependency_count; dependency_index += 1) {
        const sxml_qt4_dependency *dependency =
            &deciding.dependencies[dependency_index];
        if (sxml_xslt_test_is_spec(dependency) &&
            !sxml_xslt_test_spec_value_matches(dependency->value)) {
            sxml_xslt_test_skip(applicability,
                                (sxml_xslt_test_skip_reason){
                                    .reason = SXML_TEXT("spec"),
                                    .detail = dependency->value,
                                    .is_inverted = false});
            return false;
        }
    }
    return true;
}

/* Rule 2 for one list. */
static bool
sxml_xslt_test_decide_dependencies(sxml_qt4_dependency_view dependencies,
                                   sxml_qt4_applicability *applicability) {
    for (size_t dependency_index = 0;
         dependency_index < dependencies.dependency_count;
         dependency_index += 1) {
        const sxml_qt4_dependency *dependency =
            &dependencies.dependencies[dependency_index];
        if (sxml_xslt_test_is_spec(dependency)) {
            continue;
        }
        const bool is_supported =
            sxml_xslt_test_dependency_is_supported((sxml_qt4_dependency_value){
                .dependency_type = dependency->dependency_type,
                .value = dependency->value});
        if (is_supported != dependency->requires_satisfaction) {
            sxml_xslt_test_skip(applicability,
                                (sxml_xslt_test_skip_reason){
                                    .reason = dependency->dependency_type,
                                    .detail = dependency->value,
                                    .is_inverted = is_supported});
            return false;
        }
    }
    return true;
}

/* The collations of D97: codepoint, HTML ASCII case-insensitive, and UCA
   URIs (with fallback; the suite asks for none without). */
static bool sxml_xslt_test_collation_is_provided(sxml_byte_view uri) {
    return sxml_byte_view_equals(
               uri, SXML_TEXT("http://www.w3.org/2005/xpath-functions/"
                              "collation/codepoint")) ||
           sxml_byte_view_equals(
               uri, SXML_TEXT("http://www.w3.org/2005/xpath-functions/"
                              "collation/html-ascii-case-insensitive")) ||
           sxml_xslt_test_starts_with(
               uri, SXML_TEXT("http://www.w3.org/2013/collation/UCA"));
}

/* Rule 3: the part of the environment sxml cannot provide, or an empty
   view. */
static sxml_byte_view sxml_xslt_test_unsupported_environment_part(
    const sxml_xslt_test_environment *environment) {
    const sxml_qt4_environment *shared = &environment->shared;
    for (size_t detail_index = 0;
         detail_index < environment->source_details.source_detail_count;
         detail_index += 1) {
        const sxml_xslt_test_source_detail *detail =
            &environment->source_details.source_details[detail_index];
        if (detail->needs_xinclude) {
            return SXML_TEXT("xinclude");
        }
    }
    for (size_t collation_index = 0;
         collation_index < shared->collations.collation_count;
         collation_index += 1) {
        if (!sxml_xslt_test_collation_is_provided(
                shared->collations.collations[collation_index].uri)) {
            return SXML_TEXT("collation");
        }
    }
    return sxml_xslt_test_empty_text;
}

static void sxml_xslt_test_decide_scope(const sxml_xslt_test_case *test_case,
                                        sxml_qt4_applicability *applicability) {
    applicability->is_in_scope = true;
    if (sxml_xslt_test_group_is_non_goal(test_case->group_name)) {
        applicability->is_in_scope = false;
        applicability->non_goal_kind = SXML_TEXT("group");
        applicability->non_goal = test_case->group_name;
        return;
    }
    const sxml_qt4_dependency_view lists[2] = {test_case->test_set_dependencies,
                                               test_case->dependencies};
    for (size_t list_index = 0; list_index < 2; list_index += 1) {
        for (size_t dependency_index = 0;
             dependency_index < lists[list_index].dependency_count;
             dependency_index += 1) {
            const sxml_qt4_dependency *dependency =
                &lists[list_index].dependencies[dependency_index];
            if (dependency->requires_satisfaction &&
                sxml_byte_view_equals(dependency->dependency_type,
                                      SXML_TEXT("feature")) &&
                sxml_xslt_test_feature_is_non_goal(dependency->value)) {
                applicability->is_in_scope = false;
                applicability->non_goal_kind = SXML_TEXT("feature");
                applicability->non_goal = dependency->value;
                return;
            }
        }
    }
}

/* The feature schema_aware required, a schema or a validated source. */
static bool sxml_xslt_test_is_schema_aware(const sxml_xslt_test_case *test_case) {
    const sxml_qt4_dependency_view lists[2] = {test_case->test_set_dependencies,
                                               test_case->dependencies};
    for (size_t list_index = 0; list_index < 2; list_index += 1) {
        for (size_t dependency_index = 0;
             dependency_index < lists[list_index].dependency_count;
             dependency_index += 1) {
            const sxml_qt4_dependency *dependency =
                &lists[list_index].dependencies[dependency_index];
            if (dependency->requires_satisfaction &&
                sxml_byte_view_equals(dependency->dependency_type,
                                      SXML_TEXT("feature")) &&
                sxml_byte_view_equals(dependency->value,
                                      SXML_TEXT("schema_aware"))) {
                return true;
            }
        }
    }
    const sxml_qt4_environment *shared = &test_case->environment.shared;
    for (size_t source_index = 0; source_index < shared->sources.source_count;
         source_index += 1) {
        const sxml_byte_view validation =
            shared->sources.sources[source_index].validation;
        if (validation.byte_count != 0 &&
            !sxml_byte_view_equals(validation, SXML_TEXT("skip"))) {
            return true;
        }
    }
    return shared->schemas.schema_count != 0;
}

void sxml_xslt_test_decide_applicability(
    const sxml_xslt_test_case *test_case,
    sxml_qt4_applicability *applicability) {
    *applicability = (sxml_qt4_applicability){
        .is_applicable = false,
        .reason = sxml_xslt_test_empty_text,
        .detail = sxml_xslt_test_empty_text,
        .is_inverted = false,
        .is_in_scope = false,
        .is_schema_aware = false,
        .non_goal_kind = sxml_xslt_test_empty_text,
        .non_goal = sxml_xslt_test_empty_text};
    if (!sxml_xslt_test_decide_spec(test_case, applicability)) {
        return;
    }
    sxml_xslt_test_decide_scope(test_case, applicability);
    applicability->is_schema_aware = sxml_xslt_test_is_schema_aware(test_case);
    if (!sxml_xslt_test_decide_dependencies(test_case->test_set_dependencies,
                                            applicability) ||
        !sxml_xslt_test_decide_dependencies(test_case->dependencies,
                                            applicability)) {
        return;
    }
    const sxml_byte_view environment_part =
        sxml_xslt_test_unsupported_environment_part(&test_case->environment);
    if (environment_part.byte_count != 0) {
        sxml_xslt_test_skip(applicability,
                            (sxml_xslt_test_skip_reason){
                                .reason = SXML_TEXT("environment"),
                                .detail = environment_part,
                                .is_inverted = false});
        return;
    }
    if (test_case->has_posture_and_sweep) {
        sxml_xslt_test_skip(applicability,
                            (sxml_xslt_test_skip_reason){
                                .reason = SXML_TEXT("posture-and-sweep"),
                                .detail = sxml_xslt_test_empty_text,
                                .is_inverted = false});
        return;
    }
    applicability->is_applicable = true;
}
