/*
 * Applicability of QT4 test cases (applicability.h). The spec rule mirrors
 * tools/check-test-list.py, so a test list that the script accepts is run
 * in full.
 */
#include "applicability.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The collations of D97 (F&O 4.0, "The Unicode codepoint collation", "The
   HTML ASCII case-insensitive collation", "The Unicode Collation
   Algorithm"). */
#define SXML_QT4_CODEPOINT_COLLATION_URI                                       \
    "http://www.w3.org/2005/xpath-functions/collation/codepoint"
#define SXML_QT4_HTML_ASCII_COLLATION_URI                                      \
    "http://www.w3.org/2005/xpath-functions/collation/"                        \
    "html-ascii-case-insensitive"
#define SXML_QT4_UCA_COLLATION_URI "http://www.w3.org/2013/collation/UCA"

/* A dependency sxml satisfies (dependency_meanings.xml). */
typedef struct sxml_qt4_supported_dependency {
    sxml_byte_view dependency_type;
    sxml_byte_view value;
} sxml_qt4_supported_dependency;

/*
 * What sxml supports (D83, D108). Everything else, schema awareness, XSLT,
 * XQuery modules, serialization parameters, advanced UCA collations, other
 * languages and older Unicode versions included, is not satisfied.
 * - higherOrderFunctions: XPath 4.0 function items (D38, packet 1.21).
 * - namespace-axis: the namespace axis (D77).
 * - xml-version 1.0, 1.0:5+: XML 1.0 Fifth Edition (D28, D81); 1.1 and
 *   1.0:4- are not.
 * - xsd-version 1.1: XSD 1.1 lexical and value spaces (D66).
 * - limits big_integer: xs:integer is unbounded (D36); year_lt_0: years
 *   are int64_t with year 0 as XSD 1.1 has it (D36, D66).
 * - language and default-language en.
 * - simple-uca-fallback: UCA collation URIs with fallback (D97, packet
 *   2.13); non_empty_sequence_collection: collections of any items
 *   (D104, SEQUENCE resources); arbitraryPrecisionDecimal: xs:decimal on
 *   mpdecimal (D36); unicode-normalization-form NFC, NFD, NFKC, NFKD
 *   (packet 2.4). Phase 2 packets make them work; until then their test
 *   cases run and fail, which the D95 denominator counts anyway.
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
};

/* The non-goals of D95 that a feature dependency names; an entry ending in
   "*" matches every value it starts. */
static const sxml_byte_view sxml_qt4_non_goal_features[] = {
    SXML_BYTE_VIEW_INITIALIZER("schemaImport"),
    SXML_BYTE_VIEW_INITIALIZER("schemaValidation"),
    SXML_BYTE_VIEW_INITIALIZER("typedData"),
    SXML_BYTE_VIEW_INITIALIZER("staticTyping"),
    SXML_BYTE_VIEW_INITIALIZER("XQUpdate"),
    SXML_BYTE_VIEW_INITIALIZER("fullText*"),
    SXML_BYTE_VIEW_INITIALIZER("xpath-1.0-compatibility"),
    SXML_BYTE_VIEW_INITIALIZER("fn-transform-XSLT*"),
    SXML_BYTE_VIEW_INITIALIZER("fn-invisible-xml"),
    SXML_BYTE_VIEW_INITIALIZER("remote_http"),
    SXML_BYTE_VIEW_INITIALIZER("expath-file"),
    SXML_BYTE_VIEW_INITIALIZER("binary"),
    SXML_BYTE_VIEW_INITIALIZER("advanced-uca-fallback"),
    SXML_BYTE_VIEW_INITIALIZER("schema-location-hint"),
};

/* The non-goal test sets of D95, with "*" as above. */
static const sxml_byte_view sxml_qt4_non_goal_test_sets[] = {
    SXML_BYTE_VIEW_INITIALIZER("upd-*"),
    SXML_BYTE_VIEW_INITIALIZER("ft-*"),
    SXML_BYTE_VIEW_INITIALIZER("bin-*"),
    SXML_BYTE_VIEW_INITIALIZER("expath-*"),
    SXML_BYTE_VIEW_INITIALIZER("misc-HtmlTestSuite"),
    SXML_BYTE_VIEW_INITIALIZER("fn-parse-html"),
    SXML_BYTE_VIEW_INITIALIZER("fn-html-doc"),
    SXML_BYTE_VIEW_INITIALIZER("fn-invisible-xml"),
    SXML_BYTE_VIEW_INITIALIZER("fn-transform"),
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
    sxml_qt4_token_cursor cursor = {.text = dependency.value, .byte_index = 0};
    sxml_byte_view token = sxml_qt4_empty_text;
    while (sxml_qt4_next_token(&cursor, &token)) {
        const size_t supported_count =
            sizeof sxml_qt4_supported_dependencies /
            sizeof sxml_qt4_supported_dependencies[0];
        for (size_t supported_index = 0; supported_index < supported_count;
             supported_index += 1) {
            const sxml_qt4_supported_dependency *supported =
                &sxml_qt4_supported_dependencies[supported_index];
            if (sxml_byte_view_equals(supported->dependency_type,
                                      dependency.dependency_type) &&
                sxml_byte_view_equals(supported->value, token)) {
                return true;
            }
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
                .value = dependency->value});
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

/* True for a collation of D97: the codepoint and HTML ASCII
   case-insensitive collations, and UCA URIs unless they ask for no
   fallback. Packet 2.12's sxml_collation_is_supported (D104) answers the
   same for the library once it exists. */
static bool sxml_qt4_collation_is_provided(sxml_byte_view uri) {
    if (sxml_byte_view_equals(uri, SXML_TEXT(SXML_QT4_CODEPOINT_COLLATION_URI)) ||
        sxml_byte_view_equals(uri,
                              SXML_TEXT(SXML_QT4_HTML_ASCII_COLLATION_URI))) {
        return true;
    }
    const sxml_byte_view uca = SXML_TEXT(SXML_QT4_UCA_COLLATION_URI);
    if (!sxml_qt4_starts_with(uri, uca)) {
        return false;
    }
    if (uri.byte_count != uca.byte_count && uri.bytes[uca.byte_count] != '?') {
        return false;
    }
    const sxml_byte_view no_fallback = SXML_TEXT("fallback=no");
    for (size_t byte_index = uca.byte_count;
         byte_index + no_fallback.byte_count <= uri.byte_count;
         byte_index += 1) {
        if (sxml_qt4_starts_with(
                (sxml_byte_view){.bytes = uri.bytes + byte_index,
                                 .byte_count = uri.byte_count - byte_index},
                no_fallback)) {
            return false;
        }
    }
    return true;
}

/* Rule 3: what the environment asks for. Returns the element sxml cannot
   provide, or an empty view. Collections with a query wait for XQuery
   (packet 3.13). */
static sxml_byte_view
sxml_qt4_unsupported_environment_part(const sxml_qt4_environment *environment) {
    for (size_t source_index = 0;
         source_index < environment->sources.source_count; source_index += 1) {
        if (environment->sources.sources[source_index].validation.byte_count !=
            0) {
            return SXML_TEXT("source validation");
        }
    }
    if (environment->schemas.schema_count != 0) {
        return SXML_TEXT("schema");
    }
    for (size_t collection_index = 0;
         collection_index < environment->collections.collection_count;
         collection_index += 1) {
        const sxml_qt4_collection *collection =
            &environment->collections.collections[collection_index];
        if (collection->queries.text_count != 0) {
            return SXML_TEXT("collection query");
        }
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
    if (environment->has_sandpit) {
        return SXML_TEXT("sandpit");
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

void sxml_qt4_decide_applicability(
    const sxml_qt4_applicability_request *request,
    sxml_qt4_applicability *applicability) {
    *applicability = (sxml_qt4_applicability){.is_applicable = false,
                                              .reason = sxml_qt4_empty_text,
                                              .detail = sxml_qt4_empty_text,
                                              .is_inverted = false,
                                              .is_in_scope = false,
                                              .non_goal_kind =
                                                  sxml_qt4_empty_text,
                                              .non_goal = sxml_qt4_empty_text};
    if (!sxml_qt4_decide_spec(request, applicability)) {
        return;
    }
    sxml_qt4_decide_scope(request, applicability);
    if (!sxml_qt4_decide_dependencies(request->test_set_dependencies,
                                      applicability) ||
        !sxml_qt4_decide_dependencies(request->test_case_dependencies,
                                      applicability)) {
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
    if (request->module_count != 0) {
        sxml_qt4_skip(applicability,
                      (sxml_qt4_skip_reason){.reason = SXML_TEXT("module"),
                                             .detail = sxml_qt4_empty_text,
                                             .is_inverted = false});
        return;
    }
    applicability->is_applicable = true;
}
