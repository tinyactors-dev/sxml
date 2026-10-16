/*
 * Applicability of QT4 test cases (applicability.h). The spec rule mirrors
 * tools/check-test-list.py, so a test list that the script accepts is run
 * in full.
 */
#include "applicability.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The Unicode codepoint collation, the only one sxml provides in Phase 1
   (F&O 4.0, section 5.3.2). */
#define SXML_QT4_CODEPOINT_COLLATION_URI                                       \
    "http://www.w3.org/2005/xpath-functions/collation/codepoint"

/* A dependency sxml satisfies (dependency_meanings.xml). */
typedef struct sxml_qt4_supported_dependency {
    sxml_byte_view dependency_type;
    sxml_byte_view value;
} sxml_qt4_supported_dependency;

/*
 * What sxml supports today (D83). Everything else, schema awareness, XSLT,
 * XQuery modules, serialization parameters, collations other than the
 * codepoint one, other languages and older Unicode versions included, is
 * not satisfied.
 * - higherOrderFunctions: XPath 4.0 function items (D38, packet 1.21).
 * - namespace-axis: the namespace axis (D77).
 * - xml-version 1.0, 1.0:5+: XML 1.0 Fifth Edition (D28, D81); 1.1 and
 *   1.0:4- are not.
 * - xsd-version 1.1: XSD 1.1 lexical and value spaces (D66).
 * - limits big_integer: xs:integer is unbounded (D36); year_lt_0: years
 *   are int64_t with year 0 as XSD 1.1 has it (D36, D66).
 * - language and default-language en.
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
};

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

static void sxml_qt4_skip(sxml_qt4_applicability *applicability,
                          const sxml_qt4_applicability *value) {
    *applicability = *value;
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
                          &(sxml_qt4_applicability){.is_applicable = false,
                                                    .reason = SXML_TEXT("spec"),
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
            sxml_qt4_skip(
                applicability,
                &(sxml_qt4_applicability){.is_applicable = false,
                                          .reason = dependency->dependency_type,
                                          .detail = dependency->value,
                                          .is_inverted = is_supported});
            return false;
        }
    }
    return true;
}

/* Rule 3: what the environment asks for. Returns the element sxml cannot
   provide, or an empty view. */
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
    if (environment->resources.resource_count != 0) {
        return SXML_TEXT("resource");
    }
    if (environment->collections.collection_count != 0) {
        return SXML_TEXT("collection");
    }
    for (size_t collation_index = 0;
         collation_index < environment->collations.collation_count;
         collation_index += 1) {
        if (!sxml_byte_view_equals(
                environment->collations.collations[collation_index].uri,
                SXML_TEXT(SXML_QT4_CODEPOINT_COLLATION_URI))) {
            return SXML_TEXT("collation");
        }
    }
    if (environment->decimal_format_names.text_count != 0) {
        return SXML_TEXT("decimal-format");
    }
    if (environment->has_function_library) {
        return SXML_TEXT("function-library");
    }
    if (environment->has_sandpit) {
        return SXML_TEXT("sandpit");
    }
    return sxml_qt4_empty_text;
}

void sxml_qt4_decide_applicability(
    const sxml_qt4_applicability_request *request,
    sxml_qt4_applicability *applicability) {
    if (!sxml_qt4_decide_spec(request, applicability) ||
        !sxml_qt4_decide_dependencies(request->test_set_dependencies,
                                      applicability) ||
        !sxml_qt4_decide_dependencies(request->test_case_dependencies,
                                      applicability)) {
        return;
    }
    const sxml_byte_view environment_part =
        sxml_qt4_unsupported_environment_part(request->environment);
    if (environment_part.byte_count != 0) {
        sxml_qt4_skip(applicability, &(sxml_qt4_applicability){
                                         .is_applicable = false,
                                         .reason = SXML_TEXT("environment"),
                                         .detail = environment_part,
                                         .is_inverted = false});
        return;
    }
    if (request->module_count != 0) {
        sxml_qt4_skip(applicability,
                      &(sxml_qt4_applicability){.is_applicable = false,
                                                .reason = SXML_TEXT("module"),
                                                .detail = sxml_qt4_empty_text,
                                                .is_inverted = false});
        return;
    }
    *applicability = (sxml_qt4_applicability){.is_applicable = true,
                                              .reason = sxml_qt4_empty_text,
                                              .detail = sxml_qt4_empty_text,
                                              .is_inverted = false};
}
