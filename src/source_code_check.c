/*
 * Checking registered source code languages (include/sxml/processor.h,
 * sxml_source_code_check_language; glossary `registered_language`;
 * packet 7.23, D288). Apart from src/source_code.c, so that the
 * processor's configuration check does not link the built-in grammars.
 *
 * Golden file: src/optional_feature.c (a configuration view's check).
 */
#include "source_code_internal.h"

#include <string.h>

bool sxml_source_code_language_name_is_valid(sxml_byte_view name) {
    if (name.byte_count == 0 || name.bytes == NULL) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index += 1) {
        const uint8_t byte = name.bytes[byte_index];
        const bool is_letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        const bool is_digit = byte >= '0' && byte <= '9';
        if (!is_letter && !is_digit && byte != '_' && byte != '-' && byte != '+' && byte != '.') {
            return false;
        }
    }
    return true;
}

/* The grammar's ABI, 0 for no grammar. */
static uint32_t abi_version_of(const sxml_source_code_language *language) {
    return language->grammar == NULL ? 0 : ts_language_abi_version(language->grammar);
}

static bool abi_is_supported(uint32_t abi_version) {
    return abi_version >= SXML_SOURCE_CODE_MINIMUM_LANGUAGE_ABI &&
           abi_version <= SXML_SOURCE_CODE_MAXIMUM_LANGUAGE_ABI;
}

static bool names_equal(sxml_byte_view first, sxml_byte_view second) {
    return first.byte_count == second.byte_count &&
           memcmp(first.bytes, second.bytes, first.byte_count) == 0;
}

bool sxml_source_code_language_view_is_valid(sxml_source_code_language_view view) {
    if (view.language_count == 0) {
        return true;
    }
    if (view.languages == NULL) {
        return false;
    }
    for (size_t language_index = 0; language_index < view.language_count; language_index += 1) {
        const sxml_source_code_language *language = &view.languages[language_index];
        if (!sxml_source_code_language_name_is_valid(language->language_name) ||
            !abi_is_supported(abi_version_of(language))) {
            return false;
        }
        for (size_t other_index = 0; other_index < language_index; other_index += 1) {
            if (names_equal(view.languages[other_index].language_name, language->language_name)) {
                return false;
            }
        }
    }
    return true;
}

/* One query of a language and the problem it makes. */
typedef struct query_check {
    sxml_byte_view query_text;
    sxml_source_code_language_problem problem;
} query_check;

sxml_status sxml_source_code_check_language(const sxml_source_code_language *language,
                                            sxml_source_code_language_report *report) {
    if (language == NULL || report == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *report = (sxml_source_code_language_report){
        .problem = SXML_SOURCE_CODE_LANGUAGE_PROBLEM_NONE,
        .abi_version = abi_version_of(language),
        .byte_offset = 0};
    if (!sxml_source_code_language_name_is_valid(language->language_name)) {
        report->problem = SXML_SOURCE_CODE_LANGUAGE_PROBLEM_NAME;
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!abi_is_supported(report->abi_version)) {
        report->problem = SXML_SOURCE_CODE_LANGUAGE_PROBLEM_ABI;
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const query_check checks[] = {
        {language->highlights_query, SXML_SOURCE_CODE_LANGUAGE_PROBLEM_HIGHLIGHTS_QUERY},
        {language->locals_query, SXML_SOURCE_CODE_LANGUAGE_PROBLEM_LOCALS_QUERY},
        {language->injections_query, SXML_SOURCE_CODE_LANGUAGE_PROBLEM_INJECTIONS_QUERY},
    };
    for (size_t check_index = 0; check_index < sizeof(checks) / sizeof(checks[0]);
         check_index += 1) {
        const sxml_byte_view text = checks[check_index].query_text;
        if (text.byte_count == 0) {
            continue;
        }
        if (text.bytes == NULL || text.byte_count >= UINT32_MAX) {
            report->problem = checks[check_index].problem;
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        uint32_t error_offset = 0;
        TSQueryError error_type = TSQueryErrorNone;
        /* tree-sitter allocates the query with malloc (D12, D285). */
        TSQuery *query = ts_query_new(language->grammar, (const char *)text.bytes,
                                      (uint32_t)text.byte_count, &error_offset, &error_type);
        if (query == NULL) {
            report->problem = checks[check_index].problem;
            report->byte_offset = error_offset;
            return error_type == TSQueryErrorNone ? SXML_STATUS_OUT_OF_MEMORY
                                                  : SXML_STATUS_INVALID_ARGUMENT;
        }
        ts_query_delete(query);
    }
    return SXML_STATUS_OK;
}
