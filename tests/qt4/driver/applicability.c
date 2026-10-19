/*
 * Applicability of QT4 test cases (applicability.h). The spec rule mirrors
 * tools/check-test-list.py, so a test list that the script accepts is run
 * in full.
 */
#include "applicability.h"
#include "sxml/xpath.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* A dependency sxml satisfies (dependency_meanings.xml). */
typedef struct sxml_qt4_supported_dependency {
    sxml_byte_view dependency_type;
    sxml_byte_view value;
} sxml_qt4_supported_dependency;

/*
 * What sxml supports (D83, D108, D134). Everything else, schema
 * awareness, XSLT, advanced UCA collations, other languages and older
 * Unicode versions included, is not satisfied.
 * - higherOrderFunctions: XPath 4.0 function items (D38, packet 1.21).
 * - namespace-axis: the namespace axis (D77).
 * - xml-version 1.0, 1.0:5+ and 1.1: documents are read by XML 1.0 Fifth
 *   Edition or, when they declare version 1.1, by XML 1.1 (D28, D81,
 *   D195); 1.0:4- is not. Query texts follow the XML 1.0 rules (line
 *   ends, character references, namespace undeclaration), so the 1.1
 *   cases that need XQuery's 1.1 mode fail (D195).
 * - xsd-version 1.1: XSD 1.1 lexical and value spaces (D66).
 * - limits big_integer: xs:integer is unbounded (D36); year_lt_0: years
 *   are int64_t with year 0 as XSD 1.1 has it (D36, D66).
 * - language and default-language en.
 * - simple-uca-fallback: UCA collation URIs with fallback (D97, packet
 *   2.13); non_empty_sequence_collection: collections of any items
 *   (D104, SEQUENCE resources); arbitraryPrecisionDecimal: xs:decimal on
 *   mpdecimal (D36); unicode-normalization-form NFC, NFD, NFKC, NFKD
 *   and FULLY-NORMALIZED (packet 2.4, D107). Phase 2 packets make them work; until then their test
 *   cases run and fail, which the D95 denominator counts anyway.
 * - serialization: the serializers of packets 3.3 and 3.4, which the
 *   serialization assertions use (packet 3.13).
 * - fn-load-xquery-module, for XQuery only: the function is XQuery's
 *   (src/xquery_load_module.c, D154).
 * - schemaImport, schemaValidation and typedData: schema awareness
 *   (packet 5.11, D174); they stay non-goals of the D95 denominators
 *   below, so the Phase 2 to 4 gate rates keep their meaning, and the
 *   report counts them as the area "schema-aware".
 * - XQUpdate and revalidation skip (XQuery only): the XQuery Update
 *   Facility 3.0 (packet 7.7a, D187); revalidation lax and strict are
 *   not supported yet (XUST0026, 7.7c).
 * - binary: the EXPath Binary Module 4.0, the optional feature
 *   sxml_expath_binary_feature() that the driver's processor names
 *   (packet 7.8, D198).
 * - expath-file: the EXPath File Module 4.0, sxml_expath_file_feature()
 *   over the platform's files; a sandpit is copied for each test case
 *   (sandpit.h, packet 7.9, D200).
 */
static const sxml_qt4_supported_dependency sxml_qt4_supported_dependencies[] = {
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("higherOrderFunctions")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("namespace-axis")},
    {SXML_BYTE_VIEW_INITIALIZER("xml-version"),
     SXML_BYTE_VIEW_INITIALIZER("1.0")},
    {SXML_BYTE_VIEW_INITIALIZER("xml-version"),
     SXML_BYTE_VIEW_INITIALIZER("1.0:5+")},
    {SXML_BYTE_VIEW_INITIALIZER("xml-version"),
     SXML_BYTE_VIEW_INITIALIZER("1.1")},
    {SXML_BYTE_VIEW_INITIALIZER("xsd-version"),
     SXML_BYTE_VIEW_INITIALIZER("1.1")},
    {SXML_BYTE_VIEW_INITIALIZER("limits"),
     SXML_BYTE_VIEW_INITIALIZER("big_integer")},
    {SXML_BYTE_VIEW_INITIALIZER("limits"),
     SXML_BYTE_VIEW_INITIALIZER("year_lt_0")},
    {SXML_BYTE_VIEW_INITIALIZER("language"), SXML_BYTE_VIEW_INITIALIZER("en")},
    {SXML_BYTE_VIEW_INITIALIZER("default-language"),
     SXML_BYTE_VIEW_INITIALIZER("en")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("simple-uca-fallback")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("non_empty_sequence_collection")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("arbitraryPrecisionDecimal")},
    {SXML_BYTE_VIEW_INITIALIZER("unicode-normalization-form"),
     SXML_BYTE_VIEW_INITIALIZER("NFC")},
    {SXML_BYTE_VIEW_INITIALIZER("unicode-normalization-form"),
     SXML_BYTE_VIEW_INITIALIZER("NFD")},
    {SXML_BYTE_VIEW_INITIALIZER("unicode-normalization-form"),
     SXML_BYTE_VIEW_INITIALIZER("NFKC")},
    {SXML_BYTE_VIEW_INITIALIZER("unicode-normalization-form"),
     SXML_BYTE_VIEW_INITIALIZER("NFKD")},
    {SXML_BYTE_VIEW_INITIALIZER("unicode-normalization-form"),
     SXML_BYTE_VIEW_INITIALIZER("FULLY-NORMALIZED")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("serialization")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("schemaImport")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("schemaValidation")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("typedData")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("binary")},
    /* fn:transform, the optional feature sxml_xslt_transform_feature()
       (packet 7.11, D203): an XSLT 4.0 processor serves the requests for
       XSLT 3.0. */
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("fn-transform-XSLT")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("fn-transform-XSLT30")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("expath-file")},
    /* DTD processing (packets 7.14a to 7.14c, D199, D210): the DTD's
       infoset (is-id, is-idrefs, unparsed entities) and fn:doc's and
       fn:parse-xml's dtd-validation option. */
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("infoset-dtd")},
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("dtd-validation-option")},
    /* Olson (IANA) timezone names in the formatting functions' $place:
       the optional feature sxml_timezone_database_feature() over the
       vendored tzdata (packet 7.16, D209). */
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("olson-timezone")},
};

/* The features of schema awareness (the area "schema-aware",
   applicability.h). */
static const sxml_byte_view sxml_qt4_schema_aware_features[] = {
    SXML_BYTE_VIEW_INITIALIZER("schemaImport"),
    SXML_BYTE_VIEW_INITIALIZER("schemaValidation"),
    SXML_BYTE_VIEW_INITIALIZER("typedData"),
};

/* Supported only when the run targets XQuery. */
static const sxml_qt4_supported_dependency sxml_qt4_xquery_dependencies[] = {
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("fn-load-xquery-module")},
    /* The XQuery Update Facility 3.0 (packet 7.7a, D187), with
       every revalidation mode (packet 7.7c, D205). */
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("XQUpdate")},
    {SXML_BYTE_VIEW_INITIALIZER("revalidation"),
     SXML_BYTE_VIEW_INITIALIZER("skip")},
    {SXML_BYTE_VIEW_INITIALIZER("revalidation"),
     SXML_BYTE_VIEW_INITIALIZER("lax")},
    {SXML_BYTE_VIEW_INITIALIZER("revalidation"),
     SXML_BYTE_VIEW_INITIALIZER("strict")},
};

/* For XPath only: XPath 1.0 compatibility mode (packet 7.4,
   D197), which XQuery does not have; it stays a non-goal of the
   D95 denominators below, as schemaImport does. */
static const sxml_qt4_supported_dependency sxml_qt4_xpath_dependencies[] = {
    {SXML_BYTE_VIEW_INITIALIZER("feature"),
     SXML_BYTE_VIEW_INITIALIZER("xpath-1.0-compatibility")},
};

/* A table of supported dependencies. */
typedef struct sxml_qt4_dependency_table {
    const sxml_qt4_supported_dependency *entries;
    size_t entry_count;
} sxml_qt4_dependency_table;

static bool sxml_qt4_table_has(sxml_qt4_dependency_table table,
                               sxml_byte_view dependency_type,
                               sxml_byte_view token) {
    for (size_t entry_index = 0; entry_index < table.entry_count;
         entry_index += 1) {
        if (sxml_byte_view_equals(table.entries[entry_index].dependency_type,
                                  dependency_type) &&
            sxml_byte_view_equals(table.entries[entry_index].value, token)) {
            return true;
        }
    }
    return false;
}

/* The non-goals of D95 that a feature dependency names; an entry ending in
   "*" matches every value it starts. */
static const sxml_byte_view sxml_qt4_non_goal_features[] = {
    SXML_BYTE_VIEW_INITIALIZER("schemaImport"),
    SXML_BYTE_VIEW_INITIALIZER("schemaValidation"),
    SXML_BYTE_VIEW_INITIALIZER("typedData"),
    SXML_BYTE_VIEW_INITIALIZER("staticTyping"),
    SXML_BYTE_VIEW_INITIALIZER("fullText*"),
    SXML_BYTE_VIEW_INITIALIZER("xpath-1.0-compatibility"),
    SXML_BYTE_VIEW_INITIALIZER("fn-invisible-xml"),
    SXML_BYTE_VIEW_INITIALIZER("remote_http"),
    SXML_BYTE_VIEW_INITIALIZER("advanced-uca-fallback"),
    SXML_BYTE_VIEW_INITIALIZER("schema-location-hint"),
};

/* The non-goal test sets of D95, with "*" as above. */
static const sxml_byte_view sxml_qt4_non_goal_test_sets[] = {
    SXML_BYTE_VIEW_INITIALIZER("ft-*"),
    SXML_BYTE_VIEW_INITIALIZER("fn-invisible-xml"),
};

/* A table of names, some ending in "*". */
typedef struct sxml_qt4_name_table {
    const sxml_byte_view *names;
    size_t name_count;
} sxml_qt4_name_table;

static bool sxml_qt4_starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           sxml_byte_view_equals(
               (sxml_byte_view){.bytes = text.bytes,
                                .byte_count = prefix.byte_count},
               prefix);
}

static bool sxml_qt4_name_table_contains(sxml_qt4_name_table table,
                                         sxml_byte_view name) {
    for (size_t name_index = 0; name_index < table.name_count;
         name_index += 1) {
        const sxml_byte_view entry = table.names[name_index];
        if (entry.byte_count != 0 &&
            entry.bytes[entry.byte_count - 1] == '*') {
            if (sxml_qt4_starts_with(
                    name, (sxml_byte_view){.bytes = entry.bytes,
                                           .byte_count =
                                               entry.byte_count - 1})) {
                return true;
            }
        } else if (sxml_byte_view_equals(entry, name)) {
            return true;
        }
    }
    return false;
}

static bool sxml_qt4_is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* Walks the space-separated tokens of a value (an xs:NMTOKENS-like
   list). */
typedef struct sxml_qt4_token_cursor {
    sxml_byte_view text;
    size_t byte_index;
} sxml_qt4_token_cursor;

/* Writes the next token to *token and returns true, or returns false at
   the end. */
static bool sxml_qt4_next_token(sxml_qt4_token_cursor *cursor,
                                sxml_byte_view *token) {
    const sxml_byte_view text = cursor->text;
    while (cursor->byte_index < text.byte_count &&
           sxml_qt4_is_space(text.bytes[cursor->byte_index])) {
        cursor->byte_index += 1;
    }
    if (cursor->byte_index == text.byte_count) {
        return false;
    }
    const size_t start = cursor->byte_index;
    while (cursor->byte_index < text.byte_count &&
           !sxml_qt4_is_space(text.bytes[cursor->byte_index])) {
        cursor->byte_index += 1;
    }
    *token = (sxml_byte_view){.bytes = text.bytes + start,
                              .byte_count = cursor->byte_index - start};
    return true;
}

bool sxml_qt4_feature_is_non_goal(sxml_byte_view value) {
    const sxml_qt4_name_table table = {
        .names = sxml_qt4_non_goal_features,
        .name_count = sizeof sxml_qt4_non_goal_features /
                      sizeof sxml_qt4_non_goal_features[0]};
    sxml_qt4_token_cursor cursor = {.text = value, .byte_index = 0};
    sxml_byte_view token = sxml_qt4_empty_text;
    bool has_token = false;
    while (sxml_qt4_next_token(&cursor, &token)) {
        if (!sxml_qt4_name_table_contains(table, token)) {
            return false;
        }
        has_token = true;
    }
    return has_token;
}

bool sxml_qt4_test_set_is_non_goal(sxml_byte_view test_set_name) {
    return sxml_qt4_name_table_contains(
        (sxml_qt4_name_table){.names = sxml_qt4_non_goal_test_sets,
                              .name_count =
                                  sizeof sxml_qt4_non_goal_test_sets /
                                  sizeof sxml_qt4_non_goal_test_sets[0]},
        test_set_name);
}

bool sxml_qt4_spec_token_matches(sxml_byte_view token, sxml_qt4_target target) {
    if (token.byte_count < 3) {
        return false;
    }
    const sxml_byte_view language = {.bytes = token.bytes, .byte_count = 2};
    const sxml_byte_view wanted_language =
        target.language == SXML_QT4_LANGUAGE_XPATH ? SXML_TEXT("XP")
                                                   : SXML_TEXT("XQ");
    if (!sxml_byte_view_equals(language, wanted_language)) {
        return false;
    }
    size_t digit_end = token.byte_count;
    const bool is_or_later = token.bytes[token.byte_count - 1] == '+';
    if (is_or_later) {
        digit_end -= 1;
    }
    if (digit_end <= 2 || digit_end - 2 > 4) {
        return false;
    }
    uint32_t version = 0;
    for (size_t byte_index = 2; byte_index < digit_end; byte_index += 1) {
        const uint8_t byte = token.bytes[byte_index];
        if (byte < '0' || byte > '9') {
            return false;
        }
        version = version * 10 + (uint32_t)(byte - '0');
    }
    return version == target.version ||
           (is_or_later && version <= target.version);
}

bool sxml_qt4_dependency_is_supported(sxml_qt4_dependency_value dependency) {
    const sxml_qt4_dependency_table common = {
        .entries = sxml_qt4_supported_dependencies,
        .entry_count = sizeof sxml_qt4_supported_dependencies /
                       sizeof sxml_qt4_supported_dependencies[0]};
    const sxml_qt4_dependency_table xquery = {
        .entries = sxml_qt4_xquery_dependencies,
        .entry_count = sizeof sxml_qt4_xquery_dependencies /
                       sizeof sxml_qt4_xquery_dependencies[0]};
    const sxml_qt4_dependency_table xpath = {
        .entries = sxml_qt4_xpath_dependencies,
        .entry_count = sizeof sxml_qt4_xpath_dependencies /
                       sizeof sxml_qt4_xpath_dependencies[0]};
    sxml_qt4_token_cursor cursor = {.text = dependency.value, .byte_index = 0};
    sxml_byte_view token = sxml_qt4_empty_text;
    while (sxml_qt4_next_token(&cursor, &token)) {
        if (sxml_qt4_table_has(common, dependency.dependency_type, token) ||
            (dependency.runs_xquery &&
             sxml_qt4_table_has(xquery, dependency.dependency_type, token)) ||
            (!dependency.runs_xquery &&
             sxml_qt4_table_has(xpath, dependency.dependency_type, token))) {
            return true;
        }
    }
    return false;
}

static bool sxml_qt4_is_spec(const sxml_qt4_dependency *dependency) {
    return sxml_byte_view_equals(dependency->dependency_type,
                                 SXML_TEXT("spec"));
}

static size_t sxml_qt4_count_specs(sxml_qt4_dependency_view dependencies) {
    size_t spec_count = 0;
    for (size_t dependency_index = 0;
         dependency_index < dependencies.dependency_count;
         dependency_index += 1) {
        if (sxml_qt4_is_spec(&dependencies.dependencies[dependency_index])) {
            spec_count += 1;
        }
    }
    return spec_count;
}

static bool sxml_qt4_spec_value_matches(sxml_byte_view value,
                                        sxml_qt4_target target) {
    sxml_qt4_token_cursor cursor = {.text = value, .byte_index = 0};
    sxml_byte_view token = sxml_qt4_empty_text;
    while (sxml_qt4_next_token(&cursor, &token)) {
        if (sxml_qt4_spec_token_matches(token, target)) {
            return true;
        }
    }
    return false;
}

/* Why a test case is skipped. */
typedef struct sxml_qt4_skip_reason {
    sxml_byte_view reason;
    sxml_byte_view detail;
    bool is_inverted;
} sxml_qt4_skip_reason;

/* Marks the test case skipped; its scope stays as decided. */
static void sxml_qt4_skip(sxml_qt4_applicability *applicability,
                          sxml_qt4_skip_reason skip_reason) {
    applicability->is_applicable = false;
    applicability->reason = skip_reason.reason;
    applicability->detail = skip_reason.detail;
    applicability->is_inverted = skip_reason.is_inverted;
}

/* Rule 1: the spec. Returns true when it admits the target. */
static bool sxml_qt4_decide_spec(const sxml_qt4_applicability_request *request,
                                 sxml_qt4_applicability *applicability) {
    const sxml_qt4_dependency_view specs =
        sxml_qt4_count_specs(request->test_case_dependencies) > 0
            ? request->test_case_dependencies
            : request->test_set_dependencies;
    for (size_t dependency_index = 0; dependency_index < specs.dependency_count;
         dependency_index += 1) {
        const sxml_qt4_dependency *dependency =
            &specs.dependencies[dependency_index];
        if (sxml_qt4_is_spec(dependency) &&
            !sxml_qt4_spec_value_matches(dependency->value, request->target)) {
            sxml_qt4_skip(applicability,
                          (sxml_qt4_skip_reason){.reason = SXML_TEXT("spec"),
                                                 .detail = dependency->value,
                                                 .is_inverted = false});
            return false;
        }
    }
    return true;
}

/* Rule 2 for one list. Returns true when every dependency is as asked. */
static bool
sxml_qt4_decide_dependencies(sxml_qt4_dependency_view dependencies,
                             sxml_qt4_target target,
                             sxml_qt4_applicability *applicability) {
    for (size_t dependency_index = 0;
         dependency_index < dependencies.dependency_count;
         dependency_index += 1) {
        const sxml_qt4_dependency *dependency =
            &dependencies.dependencies[dependency_index];
        if (sxml_qt4_is_spec(dependency)) {
            continue;
        }
        const bool is_supported =
            sxml_qt4_dependency_is_supported((sxml_qt4_dependency_value){
                .dependency_type = dependency->dependency_type,
                .value = dependency->value,
                .runs_xquery = target.language == SXML_QT4_LANGUAGE_XQUERY});
        if (is_supported != dependency->requires_satisfaction) {
            sxml_qt4_skip(applicability, (sxml_qt4_skip_reason){
                                             .reason =
                                                 dependency->dependency_type,
                                             .detail = dependency->value,
                                             .is_inverted = is_supported});
            return false;
        }
    }
    return true;
}

/* True for a collation of D97, as the library answers it (D104, D116):
   the codepoint, Unicode and HTML ASCII case-insensitive collations, and
   UCA URIs unless they ask for no fallback. */
static bool sxml_qt4_collation_is_provided(sxml_byte_view uri) {
    return sxml_collation_is_supported(uri);
}

/* Rule 3: what the environment asks for. Returns the element sxml cannot
   provide, or an empty view. */
static sxml_byte_view
sxml_qt4_unsupported_environment_part(const sxml_qt4_environment *environment) {
    /* Schemas and validated sources are served (test_run.c, D174);
       sources of collections are not validated. */
    for (size_t collection_index = 0;
         collection_index < environment->collections.collection_count;
         collection_index += 1) {
        const sxml_qt4_collection *collection =
            &environment->collections.collections[collection_index];
        for (size_t source_index = 0;
             source_index < collection->sources.source_count;
             source_index += 1) {
            if (collection->sources.sources[source_index]
                    .validation.byte_count != 0) {
                return SXML_TEXT("source validation");
            }
        }
    }
    for (size_t collation_index = 0;
         collation_index < environment->collations.collation_count;
         collation_index += 1) {
        if (!sxml_qt4_collation_is_provided(
                environment->collations.collations[collation_index].uri)) {
            return SXML_TEXT("collation");
        }
    }
    if (environment->has_function_library) {
        return SXML_TEXT("function-library");
    }
    return sxml_qt4_empty_text;
}

/* The D95 scope, for a test case whose spec matches. */
static void
sxml_qt4_decide_scope(const sxml_qt4_applicability_request *request,
                      sxml_qt4_applicability *applicability) {
    applicability->is_in_scope = true;
    applicability->non_goal_kind = sxml_qt4_empty_text;
    applicability->non_goal = sxml_qt4_empty_text;
    if (sxml_qt4_test_set_is_non_goal(request->test_set_name)) {
        applicability->is_in_scope = false;
        applicability->non_goal_kind = SXML_TEXT("test-set");
        applicability->non_goal = request->test_set_name;
        return;
    }
    const sxml_qt4_dependency_view lists[2] = {request->test_set_dependencies,
                                               request->test_case_dependencies};
    for (size_t list_index = 0; list_index < 2; list_index += 1) {
        for (size_t dependency_index = 0;
             dependency_index < lists[list_index].dependency_count;
             dependency_index += 1) {
            const sxml_qt4_dependency *dependency =
                &lists[list_index].dependencies[dependency_index];
            if (dependency->requires_satisfaction &&
                sxml_byte_view_equals(dependency->dependency_type,
                                      SXML_TEXT("feature")) &&
                sxml_qt4_feature_is_non_goal(dependency->value)) {
                applicability->is_in_scope = false;
                applicability->non_goal_kind = SXML_TEXT("feature");
                applicability->non_goal = dependency->value;
                return;
            }
        }
    }
}

/* True when a required feature dependency names schema awareness, or
   the environment has a schema or a validated source. */
static bool
sxml_qt4_is_schema_aware(const sxml_qt4_applicability_request *request) {
    const sxml_qt4_name_table table = {
        .names = sxml_qt4_schema_aware_features,
        .name_count = sizeof sxml_qt4_schema_aware_features /
                      sizeof sxml_qt4_schema_aware_features[0]};
    const sxml_qt4_dependency_view lists[2] = {request->test_set_dependencies,
                                               request->test_case_dependencies};
    for (size_t list_index = 0; list_index < 2; list_index += 1) {
        for (size_t dependency_index = 0;
             dependency_index < lists[list_index].dependency_count;
             dependency_index += 1) {
            const sxml_qt4_dependency *dependency =
                &lists[list_index].dependencies[dependency_index];
            if (!dependency->requires_satisfaction ||
                !sxml_byte_view_equals(dependency->dependency_type,
                                       SXML_TEXT("feature"))) {
                continue;
            }
            sxml_qt4_token_cursor cursor = {.text = dependency->value,
                                            .byte_index = 0};
            sxml_byte_view token = sxml_qt4_empty_text;
            while (sxml_qt4_next_token(&cursor, &token)) {
                if (sxml_qt4_name_table_contains(table, token)) {
                    return true;
                }
            }
        }
    }
    const sxml_qt4_environment *environment = request->environment;
    if (environment->schemas.schema_count != 0) {
        return true;
    }
    for (size_t source_index = 0;
         source_index < environment->sources.source_count; source_index += 1) {
        if (environment->sources.sources[source_index].validation.byte_count !=
            0) {
            return true;
        }
    }
    return false;
}

void sxml_qt4_decide_applicability(
    const sxml_qt4_applicability_request *request,
    sxml_qt4_applicability *applicability) {
    *applicability = (sxml_qt4_applicability){.is_applicable = false,
                                              .reason = sxml_qt4_empty_text,
                                              .detail = sxml_qt4_empty_text,
                                              .is_inverted = false,
                                              .is_in_scope = false,
                                              .is_schema_aware = false,
                                              .non_goal_kind =
                                                  sxml_qt4_empty_text,
                                              .non_goal = sxml_qt4_empty_text};
    if (!sxml_qt4_decide_spec(request, applicability)) {
        return;
    }
    sxml_qt4_decide_scope(request, applicability);
    applicability->is_schema_aware = sxml_qt4_is_schema_aware(request);
    if (!sxml_qt4_decide_dependencies(request->test_set_dependencies,
                                      request->target, applicability) ||
        !sxml_qt4_decide_dependencies(request->test_case_dependencies,
                                      request->target, applicability)) {
        return;
    }
    const sxml_byte_view environment_part =
        sxml_qt4_unsupported_environment_part(request->environment);
    if (environment_part.byte_count != 0) {
        sxml_qt4_skip(applicability, (sxml_qt4_skip_reason){
                                         .reason = SXML_TEXT("environment"),
                                         .detail = environment_part,
                                         .is_inverted = false});
        return;
    }
    applicability->is_applicable = true;
}
