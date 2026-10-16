#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sxml/status.h"
#include "sxml/view.h"

/*
 * "Did you mean" suggestions for mistyped commands, options and option
 * values (docs/CLI.md, "One intent, one action"). A suggestion is only ever
 * shown; the program never acts on it.
 *
 * Names are compared by edit distance (optimal string alignment: insertion,
 * deletion, substitution and transposition of adjacent bytes each cost 1),
 * with ASCII letters compared case-insensitively.
 *
 * Thread safety: safe to call from any thread.
 */

/* Longer names get no edit distance and so no suggestion. */
#define SXML_CLI_EDIT_DISTANCE_MAXIMUM_BYTE_COUNT 64

/* Two texts that are compared. */
typedef struct sxml_cli_text_pair {
    sxml_byte_view first;
    sxml_byte_view second;
} sxml_cli_text_pair;

/*
 * Writes the edit distance between pair.first and pair.second to *distance.
 * Returns SXML_STATUS_LIMIT_EXCEEDED, leaving *distance untouched, when
 * either text is longer than SXML_CLI_EDIT_DISTANCE_MAXIMUM_BYTE_COUNT.
 */
SXML_NODISCARD sxml_status sxml_cli_edit_distance(sxml_cli_text_pair pair,
                                                  size_t *distance);

/*
 * The search for the candidate closest to what the user typed. Start it with
 * sxml_cli_suggestion_start, pass every candidate to
 * sxml_cli_suggestion_consider, then read best_candidate.
 *
 * A candidate is close enough when its distance is at most 1 for a typed
 * text of up to 3 bytes and at most 2 otherwise, and smaller than the
 * candidate's own length; or when the typed text is a prefix of it (which
 * ranks as the largest distance allowed). Of the close candidates the one
 * with the smallest distance wins, the first one on a tie.
 */
typedef struct sxml_cli_suggestion {
    /* Borrowed: what the user typed. */
    sxml_byte_view typed_text;
    bool has_candidate;
    /* Borrowed from the candidate passed in; empty without a candidate. */
    sxml_byte_view best_candidate;
    size_t best_distance;
} sxml_cli_suggestion;

/* A search for typed_text that has no candidate yet. */
sxml_cli_suggestion sxml_cli_suggestion_start(sxml_byte_view typed_text);

/* Makes candidate the best candidate if it is close enough and closer. */
void sxml_cli_suggestion_consider(sxml_cli_suggestion *suggestion,
                                  sxml_byte_view candidate);
