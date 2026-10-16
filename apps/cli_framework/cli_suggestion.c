/*
 * Edit distance and "did you mean" suggestions (cli_suggestion.h).
 *
 * Optimal string alignment distance (the restricted Damerau-Levenshtein
 * distance), computed with three rows of a dynamic programming table in
 * fixed-size arrays: names are short, so no allocation is needed.
 */
#include "cli_suggestion.h"

#include <stdint.h>

enum { SXML_CLI_EDIT_DISTANCE_ROW_COUNT = SXML_CLI_EDIT_DISTANCE_MAXIMUM_BYTE_COUNT + 1 };

static uint8_t sxml_ascii_lower_case(uint8_t byte) {
    if (byte >= 'A' && byte <= 'Z') {
        return (uint8_t)(byte - 'A' + 'a');
    }
    return byte;
}

static bool sxml_bytes_match(uint8_t first_byte, uint8_t second_byte) {
    return sxml_ascii_lower_case(first_byte) ==
           sxml_ascii_lower_case(second_byte);
}

static size_t sxml_size_minimum(size_t first_size, size_t second_size) {
    return first_size < second_size ? first_size : second_size;
}

sxml_status sxml_cli_edit_distance(sxml_cli_text_pair pair, size_t *distance) {
    const sxml_byte_view first = pair.first;
    const sxml_byte_view second = pair.second;
    if (first.byte_count > SXML_CLI_EDIT_DISTANCE_MAXIMUM_BYTE_COUNT ||
        second.byte_count > SXML_CLI_EDIT_DISTANCE_MAXIMUM_BYTE_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    /* Row r holds the distances between the first r bytes of first and
       every prefix of second; three rows are needed for transpositions. */
    size_t row_before_previous[SXML_CLI_EDIT_DISTANCE_ROW_COUNT] = {0};
    size_t previous_row[SXML_CLI_EDIT_DISTANCE_ROW_COUNT] = {0};
    size_t current_row[SXML_CLI_EDIT_DISTANCE_ROW_COUNT] = {0};
    for (size_t second_index = 0; second_index <= second.byte_count;
         second_index += 1) {
        previous_row[second_index] = second_index;
    }
    for (size_t first_index = 1; first_index <= first.byte_count;
         first_index += 1) {
        current_row[0] = first_index;
        for (size_t second_index = 1; second_index <= second.byte_count;
             second_index += 1) {
            const uint8_t first_byte = first.bytes[first_index - 1];
            const uint8_t second_byte = second.bytes[second_index - 1];
            const size_t substitution_cost =
                sxml_bytes_match(first_byte, second_byte) ? 0 : 1;
            size_t best = sxml_size_minimum(
                previous_row[second_index] + 1,
                current_row[second_index - 1] + 1);
            best = sxml_size_minimum(
                best, previous_row[second_index - 1] + substitution_cost);
            const bool is_transposition =
                first_index > 1 && second_index > 1 &&
                sxml_bytes_match(first_byte, second.bytes[second_index - 2]) &&
                sxml_bytes_match(first.bytes[first_index - 2], second_byte);
            if (is_transposition) {
                best = sxml_size_minimum(
                    best, row_before_previous[second_index - 2] + 1);
            }
            current_row[second_index] = best;
        }
        for (size_t second_index = 0; second_index <= second.byte_count;
             second_index += 1) {
            row_before_previous[second_index] = previous_row[second_index];
            previous_row[second_index] = current_row[second_index];
        }
    }
    *distance = previous_row[second.byte_count];
    return SXML_STATUS_OK;
}

sxml_cli_suggestion sxml_cli_suggestion_start(sxml_byte_view typed_text) {
    return (sxml_cli_suggestion){
        .typed_text = typed_text,
        .has_candidate = false,
        .best_candidate = {.bytes = NULL, .byte_count = 0},
        .best_distance = 0,
    };
}

static bool sxml_is_case_insensitive_prefix(sxml_cli_text_pair pair) {
    if (pair.first.byte_count == 0 ||
        pair.first.byte_count > pair.second.byte_count) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < pair.first.byte_count;
         byte_index += 1) {
        if (!sxml_bytes_match(pair.first.bytes[byte_index],
                              pair.second.bytes[byte_index])) {
            return false;
        }
    }
    return true;
}

void sxml_cli_suggestion_consider(sxml_cli_suggestion *suggestion,
                                  sxml_byte_view candidate) {
    const size_t allowed_distance =
        suggestion->typed_text.byte_count <= 3 ? 1 : 2;
    const sxml_cli_text_pair pair = {
        .first = suggestion->typed_text,
        .second = candidate,
    };
    size_t distance = 0;
    const sxml_status status = sxml_cli_edit_distance(pair, &distance);
    if (status != SXML_STATUS_OK) {
        return;
    }
    bool is_close = distance <= allowed_distance &&
                    distance < candidate.byte_count;
    if (!is_close && sxml_is_case_insensitive_prefix(pair)) {
        is_close = true;
        distance = allowed_distance;
    }
    if (!is_close) {
        return;
    }
    if (suggestion->has_candidate && distance >= suggestion->best_distance) {
        return;
    }
    suggestion->has_candidate = true;
    suggestion->best_candidate = candidate;
    suggestion->best_distance = distance;
}
