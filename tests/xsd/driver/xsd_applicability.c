/*
 * Whether an XSD test runs (xsd_applicability.h). Imitates
 * tests/xslt/driver/xslt_applicability.c: tables of supported values, a
 * decision with a reason; the token classes are xsts.xsd's known-token
 * member types.
 */
#include "xsd_applicability.h"

#include <stddef.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xsd_test_empty_text = {.bytes = NULL,
                                                        .byte_count = 0};

/* One token of xsts.xsd's known-token, its class (the index of its member
   type) and whether the driver supports it. */
typedef struct sxml_xsd_test_known_token {
    sxml_byte_view token;
    unsigned token_class;
    bool is_supported;
} sxml_xsd_test_known_token;

enum {
    SXML_XSD_TEST_CLASS_XSD_VERSION,
    SXML_XSD_TEST_CLASS_XSD_EDITION,
    SXML_XSD_TEST_CLASS_XML_SUBSTRATE,
    SXML_XSD_TEST_CLASS_UNICODE_VERSION,
    SXML_XSD_TEST_CLASS_RUNTIME_SCHEMA_ERROR,
    SXML_XSD_TEST_CLASS_XPATH_IN_CTA,
    SXML_XSD_TEST_CLASS_XDM_FILTERING,
    /* A token of no known class is a class of its own. */
    SXML_XSD_TEST_CLASS_UNKNOWN,
};

static const sxml_xsd_test_known_token sxml_xsd_test_known_tokens[] = {
    {SXML_BYTE_VIEW_INITIALIZER("1.0"), SXML_XSD_TEST_CLASS_XSD_VERSION, false},
    {SXML_BYTE_VIEW_INITIALIZER("1.1"), SXML_XSD_TEST_CLASS_XSD_VERSION, true},
    {SXML_BYTE_VIEW_INITIALIZER("1.0-1e"), SXML_XSD_TEST_CLASS_XSD_EDITION,
     false},
    {SXML_BYTE_VIEW_INITIALIZER("1.0-2e"), SXML_XSD_TEST_CLASS_XSD_EDITION,
     false},
    {SXML_BYTE_VIEW_INITIALIZER("XML-1.0"), SXML_XSD_TEST_CLASS_XML_SUBSTRATE,
     true},
    {SXML_BYTE_VIEW_INITIALIZER("XML-1.0-1e-4e"),
     SXML_XSD_TEST_CLASS_XML_SUBSTRATE, false},
    {SXML_BYTE_VIEW_INITIALIZER("XML-1.0-5e"),
     SXML_XSD_TEST_CLASS_XML_SUBSTRATE, true},
    {SXML_BYTE_VIEW_INITIALIZER("XML-1.1"), SXML_XSD_TEST_CLASS_XML_SUBSTRATE,
     false},
    {SXML_BYTE_VIEW_INITIALIZER("Unicode_4.0.0"),
     SXML_XSD_TEST_CLASS_UNICODE_VERSION, false},
    {SXML_BYTE_VIEW_INITIALIZER("Unicode_6.0.0"),
     SXML_XSD_TEST_CLASS_UNICODE_VERSION, true},
    {SXML_BYTE_VIEW_INITIALIZER("CTR-all-compile"),
     SXML_XSD_TEST_CLASS_RUNTIME_SCHEMA_ERROR, true},
    {SXML_BYTE_VIEW_INITIALIZER("CTR-all-runtime"),
     SXML_XSD_TEST_CLASS_RUNTIME_SCHEMA_ERROR, false},
    {SXML_BYTE_VIEW_INITIALIZER("CTR-all-idep"),
     SXML_XSD_TEST_CLASS_RUNTIME_SCHEMA_ERROR, false},
    {SXML_BYTE_VIEW_INITIALIZER("restricted-xpath-in-CTA"),
     SXML_XSD_TEST_CLASS_XPATH_IN_CTA, false},
    {SXML_BYTE_VIEW_INITIALIZER("full-xpath-in-CTA"),
     SXML_XSD_TEST_CLASS_XPATH_IN_CTA, true},
    {SXML_BYTE_VIEW_INITIALIZER("comments-and-PIs-excluded"),
     SXML_XSD_TEST_CLASS_XDM_FILTERING, true},
    {SXML_BYTE_VIEW_INITIALIZER("comments-and-PIs-included"),
     SXML_XSD_TEST_CLASS_XDM_FILTERING, false},
};

enum {
    SXML_XSD_TEST_KNOWN_TOKEN_COUNT =
        sizeof sxml_xsd_test_known_tokens / sizeof sxml_xsd_test_known_tokens[0]
};

static const sxml_xsd_test_known_token *
sxml_xsd_test_find_token(sxml_byte_view token) {
    for (size_t token_index = 0; token_index < SXML_XSD_TEST_KNOWN_TOKEN_COUNT;
         token_index += 1) {
        if (sxml_byte_view_equals(sxml_xsd_test_known_tokens[token_index].token,
                                  token)) {
            return &sxml_xsd_test_known_tokens[token_index];
        }
    }
    return NULL;
}

static bool sxml_xsd_test_is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* Walks the whitespace-separated tokens of a version attribute. */
typedef struct sxml_xsd_test_token_cursor {
    sxml_byte_view text;
    size_t position;
} sxml_xsd_test_token_cursor;

static bool sxml_xsd_test_next_token(sxml_xsd_test_token_cursor *cursor,
                                     sxml_byte_view *token) {
    const sxml_byte_view text = cursor->text;
    while (cursor->position < text.byte_count &&
           sxml_xsd_test_is_space(text.bytes[cursor->position])) {
        cursor->position += 1;
    }
    if (cursor->position == text.byte_count) {
        return false;
    }
    const size_t start = cursor->position;
    while (cursor->position < text.byte_count &&
           !sxml_xsd_test_is_space(text.bytes[cursor->position])) {
        cursor->position += 1;
    }
    *token = (sxml_byte_view){.bytes = text.bytes + start,
                              .byte_count = cursor->position - start};
    return true;
}

bool sxml_xsd_test_version_applies(sxml_byte_view version) {
    /* The classes the value names, and those of them it names a supported
       token of. An unknown token stands for a class of its own that is
       never supported. */
    bool is_named[SXML_XSD_TEST_CLASS_UNKNOWN + 1] = {false};
    bool is_met[SXML_XSD_TEST_CLASS_UNKNOWN + 1] = {false};
    sxml_xsd_test_token_cursor cursor = {.text = version, .position = 0};
    sxml_byte_view token = sxml_xsd_test_empty_text;
    while (sxml_xsd_test_next_token(&cursor, &token)) {
        const sxml_xsd_test_known_token *known =
            sxml_xsd_test_find_token(token);
        const unsigned token_class =
            known == NULL ? SXML_XSD_TEST_CLASS_UNKNOWN : known->token_class;
        is_named[token_class] = true;
        if (known != NULL && known->is_supported) {
            is_met[token_class] = true;
        }
    }
    for (unsigned token_class = 0; token_class <= SXML_XSD_TEST_CLASS_UNKNOWN;
         token_class += 1) {
        if (is_named[token_class] && !is_met[token_class]) {
            return false;
        }
    }
    return true;
}

bool sxml_xsd_test_version_is_supported(sxml_byte_view version) {
    sxml_xsd_test_token_cursor cursor = {.text = version, .position = 0};
    sxml_byte_view token = sxml_xsd_test_empty_text;
    while (sxml_xsd_test_next_token(&cursor, &token)) {
        const sxml_xsd_test_known_token *known =
            sxml_xsd_test_find_token(token);
        if (known == NULL || !known->is_supported) {
            return false;
        }
    }
    return true;
}

/* The statuses that leave a test out (xsts.xsd, "status"). */
static bool sxml_xsd_test_status_is_excluded(sxml_byte_view status) {
    return sxml_byte_view_equals(status, SXML_TEXT("queried")) ||
           sxml_byte_view_equals(status, SXML_TEXT("disputed-test")) ||
           sxml_byte_view_equals(status, SXML_TEXT("disputed-spec"));
}

/* The expected element that applies: the first whose tokens are all
   supported, else the first without a version; NULL for none. */
static const sxml_xsd_test_expectation *
sxml_xsd_test_choose_expectation(const sxml_xsd_test_case *test_case) {
    const sxml_xsd_test_expectation_view view = test_case->expectations;
    for (size_t expectation_index = 0;
         expectation_index < view.expectation_count; expectation_index += 1) {
        const sxml_xsd_test_expectation *expectation =
            &view.expectations[expectation_index];
        if (expectation->version.byte_count != 0 &&
            sxml_xsd_test_version_is_supported(expectation->version)) {
            return expectation;
        }
    }
    for (size_t expectation_index = 0;
         expectation_index < view.expectation_count; expectation_index += 1) {
        if (view.expectations[expectation_index].version.byte_count == 0) {
            return &view.expectations[expectation_index];
        }
    }
    return NULL;
}

/* Marks the decision not applicable for reason and detail. */
static void sxml_xsd_test_skip(sxml_xsd_test_decision *decision,
                               sxml_byte_view reason, sxml_byte_view detail) {
    decision->applicability.is_applicable = false;
    decision->applicability.is_in_scope = false;
    decision->applicability.reason = reason;
    decision->applicability.detail = detail;
}

void sxml_xsd_test_decide(const sxml_xsd_test_case *test_case,
                          sxml_xsd_test_decision *decision) {
    *decision = (sxml_xsd_test_decision){
        .applicability = {.is_applicable = true,
                          .reason = sxml_xsd_test_empty_text,
                          .detail = sxml_xsd_test_empty_text,
                          .is_inverted = false,
                          .is_in_scope = true,
                          .non_goal_kind = sxml_xsd_test_empty_text,
                          .non_goal = sxml_xsd_test_empty_text},
        .expected_validity = SXML_XSD_TEST_VALIDITY_VALID,
        .expected_text = sxml_xsd_test_empty_text};
    /* D165: the introspection set checks the suite's metadata. */
    if (sxml_byte_view_equals(test_case->test_set_file_path,
                              SXML_TEXT("common/introspection.testSet"))) {
        sxml_xsd_test_skip(decision, SXML_TEXT("test set"),
                           SXML_TEXT("introspection (the suite's metadata)"));
        return;
    }
    const sxml_byte_view versions[] = {test_case->test_set_version,
                                       test_case->group_version,
                                       test_case->test_version};
    for (size_t version_index = 0;
         version_index < sizeof versions / sizeof versions[0];
         version_index += 1) {
        if (!sxml_xsd_test_version_applies(versions[version_index])) {
            sxml_xsd_test_skip(decision, SXML_TEXT("version"),
                               versions[version_index]);
            return;
        }
    }
    if (sxml_xsd_test_status_is_excluded(test_case->status)) {
        sxml_xsd_test_skip(decision, SXML_TEXT("status"), test_case->status);
        return;
    }
    const sxml_xsd_test_expectation *expectation =
        sxml_xsd_test_choose_expectation(test_case);
    if (expectation == NULL) {
        sxml_xsd_test_skip(decision, SXML_TEXT("expected"),
                           SXML_TEXT("(none applies)"));
        return;
    }
    decision->expected_text = expectation->validity;
    if (sxml_byte_view_equals(expectation->validity, SXML_TEXT("valid"))) {
        decision->expected_validity = SXML_XSD_TEST_VALIDITY_VALID;
    } else if (sxml_byte_view_equals(expectation->validity,
                                     SXML_TEXT("invalid"))) {
        decision->expected_validity = SXML_XSD_TEST_VALIDITY_INVALID;
    } else {
        sxml_xsd_test_skip(decision, SXML_TEXT("expected"),
                           expectation->validity);
    }
}
