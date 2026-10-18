/*
 * The regular expression engine (src/regex_internal.h; docs/DECISIONS.md
 * D9, D48, D53 and D105): a hand-written parser that compiles the regex
 * into instructions, and a backtracking matcher that runs them with an
 * explicit stack, after the design of Saxon HE's engine (D9). Nothing here
 * recurses (D71): the parser keeps open groups and assertions on a stack
 * of frames, a chain of class subtractions is read and matched in a loop,
 * and the matcher keeps its choices, and the start of every lookaround
 * assertion it is in, on the backtrack stack.
 *
 * Imitates the golden module src/memory.c (lifecycle, requirements by
 * sxml_memory_requirements_append, checked sizes). The syntax is that of
 * F&O 4.0, section 6.1.3 "Regular expression grammar", which builds on
 * XSD 1.1 Part 2, appendix G "Regular Expressions".
 *
 * Compilation runs the parser twice over the same code: a counting pass
 * (regex NULL) that finds the exact number of instructions, classes and
 * class items, and an emitting pass that writes them into memory laid out
 * from those counts. Every atom is parsed and emitted once; a quantifier
 * then inserts what goes before the atom and appends copies of the
 * atom's instructions, so a small counted repetition costs one copy per
 * repetition, not a parse. A counted repetition whose copies would pass
 * SXML_REGEX_LARGEST_EXPANSION instructions becomes a counted loop
 * instead (D147): the atom once, run while a counter slot counts the
 * iterations, so a{2147483647} takes six instructions.
 */
#include "regex_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static sxml_status sxml_regex_raise_error(sxml_error_record *error_record,
                                          sxml_byte_view error_local_name,
                                          sxml_byte_view description) {
    if (error_record != NULL) {
        *error_record = (sxml_error_record){
            .code =
                {
                    .namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(
                        SXML_ERROR_NAMESPACE_URI),
                    .local_name = error_local_name,
                    .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err"),
                },
            .value = NULL,
            .diagnostic =
                {
                    .description = description,
                    .location = {.line_number = 0},
                },
        };
    }
    return SXML_STATUS_DYNAMIC_ERROR;
}

/* F&O 4.0, section 6.2: an invalid $flags is FORX0001. */
#define SXML_REGEX_FLAGS_ERROR SXML_BYTE_VIEW_FROM_LITERAL("FORX0001")
/* F&O 4.0, section 6.1: an invalid regex is FORX0002. */
#define SXML_REGEX_SYNTAX_ERROR SXML_BYTE_VIEW_FROM_LITERAL("FORX0002")

/* ----------------------------------------------------------------- Flags */

sxml_status sxml_regex_parse_flags(const sxml_regex_flags_request *request,
                                   sxml_regex_flags *flags) {
    sxml_regex_flags parsed_flags = {.is_dot_all = false};
    sxml_codepoint_iterator iterator = {.string = request->flags_text,
                                        .byte_offset = 0};
    uint32_t codepoint = 0;
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        switch (codepoint) {
        case 's':
            parsed_flags.is_dot_all = true;
            break;
        case 'm':
            parsed_flags.is_multi_line = true;
            break;
        case 'i':
            parsed_flags.is_case_insensitive = true;
            break;
        case 'x':
            parsed_flags.is_whitespace_removed = true;
            break;
        case 'q':
            parsed_flags.is_literal = true;
            break;
        case 'c':
            parsed_flags.has_comments = true;
            break;
        default:
            return sxml_regex_raise_error(
                request->error_record, SXML_REGEX_FLAGS_ERROR,
                SXML_BYTE_VIEW_FROM_LITERAL(
                    "Invalid regular expression flags: the allowed flags "
                    "are s, m, i, x, q and c"));
        }
    }
    *flags = parsed_flags;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Compiled regex */

typedef enum sxml_regex_operation {
    /* Matches the codepoint operand. */
    SXML_REGEX_OPERATION_CODEPOINT,
    /* Matches a codepoint of class operand. */
    SXML_REGEX_OPERATION_CLASS,
    /* "." without the s flag: any codepoint but newline and carriage
       return (F&O 4.0, section 6.2, flag s). */
    SXML_REGEX_OPERATION_ANY_BUT_NEWLINE,
    /* "." with the s flag. */
    SXML_REGEX_OPERATION_ANY,
    /* Continues with the next instruction; on failure, at operand. */
    SXML_REGEX_OPERATION_SPLIT_NEXT_FIRST,
    /* Continues at operand; on failure, with the next instruction. */
    SXML_REGEX_OPERATION_SPLIT_TARGET_FIRST,
    /* Continues at operand. */
    SXML_REGEX_OPERATION_JUMP,
    /* Stores the position in slot operand. */
    SXML_REGEX_OPERATION_SAVE,
    /* Fails when the position equals slot operand: a loop iteration that
       consumed nothing, which would otherwise repeat forever. */
    SXML_REGEX_OPERATION_CHECK_PROGRESS,
    /* "^" and "$" without and with the m flag (section 6.1.8.1). */
    SXML_REGEX_OPERATION_STRING_START,
    SXML_REGEX_OPERATION_STRING_END,
    SXML_REGEX_OPERATION_LINE_START,
    SXML_REGEX_OPERATION_LINE_END,
    /* Matches what group operand captured (section 6.1.6); with the i
       flag, its case variants too (section 6.2). */
    SXML_REGEX_OPERATION_BACK_REFERENCE,
    /* \b and \B (section 6.1.8.2). */
    SXML_REGEX_OPERATION_WORD_BOUNDARY,
    SXML_REGEX_OPERATION_NOT_WORD_BOUNDARY,
    /* Starts a lookahead or lookbehind assertion (sections 6.1.8.3 to
       6.1.8.6): marks the position on the backtrack stack and runs the
       assertion's regex, which ends with LOOK_END. operand is the
       instruction after that LOOK_END, where a negative assertion goes on
       when its regex fails. */
    SXML_REGEX_OPERATION_POSITIVE_LOOK_START,
    SXML_REGEX_OPERATION_NEGATIVE_LOOK_START,
    /* The regex of the innermost assertion has matched: a positive one
       goes on at the marked position, keeping what its groups captured
       but none of its other choices; a negative one fails. */
    SXML_REGEX_OPERATION_LOOK_END,
    /* Moves back operand codepoints: the start of a branch of a
       lookbehind assertion, whose branches are fixed-length (section
       6.1.8.5). */
    SXML_REGEX_OPERATION_STEP_BACK,
    /*
     * A counted loop (D147), x{n,m} with x run once per iteration;
     * operand is the loop's index in counted_loops, except for
     * COUNTED_LOOP_END. The code is
     *     COUNTED_LOOP_START   counter := 0
     *  L: COUNTED_LOOP_CHOOSE  go on at L+1 (out) or L+2 (another x)
     *     JUMP out
     *     COUNTED_LOOP_BODY    progress := position
     *     x
     *     COUNTED_LOOP_END L   counter := counter + 1, back to L
     *  out:
     * CHOOSE takes another iteration while the counter is below the
     * minimum, offers one (greedy: first; reluctant: second, section
     * 6.1.4) while it is below the maximum, and leaves at the maximum.
     * END treats an iteration that consumed nothing like the unbounded
     * loops do (CHECK_PROGRESS): above the minimum it fails; below it, it
     * stands for all the iterations still needed, which would match
     * nothing again, so the counter jumps to the minimum. Counter and
     * progress are slots, changed with restoring entries like SAVE, so
     * backtracking into an earlier iteration finds its counter.
     */
    SXML_REGEX_OPERATION_COUNTED_LOOP_START,
    SXML_REGEX_OPERATION_COUNTED_LOOP_CHOOSE,
    SXML_REGEX_OPERATION_COUNTED_LOOP_BODY,
    /* operand: the loop's COUNTED_LOOP_CHOOSE, whose operand names the
       loop. */
    SXML_REGEX_OPERATION_COUNTED_LOOP_END,
    /* The whole regex has matched. */
    SXML_REGEX_OPERATION_MATCH,
} sxml_regex_operation;

typedef struct sxml_regex_instruction {
    sxml_regex_operation operation;
    uint32_t operand;
} sxml_regex_instruction;

typedef enum sxml_regex_class_item_kind {
    /* A character, a character range or a block escape (\p{IsX}). */
    SXML_REGEX_CLASS_ITEM_KIND_RANGE,
    /* \s: space, tab, newline, carriage return. */
    SXML_REGEX_CLASS_ITEM_KIND_SPACE,
    /* \d. */
    SXML_REGEX_CLASS_ITEM_KIND_DECIMAL_DIGIT,
    /* \w. */
    SXML_REGEX_CLASS_ITEM_KIND_WORD_CHARACTER,
    /* \i: NameStartChar of XML 1.0 (fifth edition), section 2.3 (XSD 1.1
       Part 2, appendix G, "Multi-character escapes"). */
    SXML_REGEX_CLASS_ITEM_KIND_NAME_START_CHARACTER,
    /* \c: NameChar of the same section. */
    SXML_REGEX_CLASS_ITEM_KIND_NAME_CHARACTER,
    /* A category escape (\p{Lu}): the categories of category_mask. */
    SXML_REGEX_CLASS_ITEM_KIND_CATEGORY,
} sxml_regex_class_item_kind;

/* One part of a character group: a range or a class escape. */
typedef struct sxml_regex_class_item {
    sxml_regex_class_item_kind kind;
    /* \S, \D, \W, \I, \C, \P{..}. */
    bool is_complemented;
    /* RANGE only: a range of the regex under the i flag, which also
       matches the case variants of its characters (section 6.2); never
       set for block escapes. */
    bool is_case_insensitive;
    /* RANGE only. */
    uint32_t first_codepoint;
    uint32_t last_codepoint;
    /* CATEGORY only: bit c for sxml_regex_general_category c. */
    uint32_t category_mask;
} sxml_regex_class_item;

/* A character class: items, negation, and an optional subtraction. */
typedef struct sxml_regex_class {
    uint32_t first_item_index;
    uint32_t item_count;
    bool is_negated;
    /* The class subtracted ([a-z-[aeiou]]), plus one; 0 for none. */
    uint32_t subtracted_class_number;
} sxml_regex_class;

/*
 * A counted loop (D147): the quantity of x{n,m} and the two slots it
 * uses. Copies of an atom that contains the loop (the expansion of an
 * outer quantifier) share the entry and its slots: copies run one after
 * the other, and every change to the slots is undone in order when
 * matching backtracks.
 */
typedef struct sxml_regex_counted_loop {
    uint32_t minimum_count;
    /* Unused when is_unbounded. */
    uint32_t maximum_count;
    bool is_unbounded;
    bool is_reluctant;
    /* The iterations finished. */
    uint32_t counter_slot;
    /* Where the iteration under way started. */
    uint32_t progress_slot;
} sxml_regex_counted_loop;

/* What the compiled regex knows of a capturing group. */
typedef struct sxml_regex_group {
    /* The name of a named capturing subexpression, in group_names; 0 and
       0 for an unnamed group. */
    uint32_t name_offset;
    uint32_t name_byte_count;
    /* The innermost capturing group around this one; 0 for none. */
    uint32_t parent_group_number;
    /* Within a lookahead assertion (section 6.1.8.3). */
    bool is_in_lookahead;
} sxml_regex_group;

struct sxml_regex {
    sxml_regex_flags flags;
    uint32_t group_count;
    /* Two slots per group, group 0 included, then one per progress
       register of an unbounded loop and two per counted loop. */
    uint32_t slot_count;
    uint32_t instruction_count;
    uint32_t class_count;
    uint32_t class_item_count;
    uint32_t counted_loop_count;
    uint32_t group_name_byte_count;
    sxml_regex_instruction *instructions;
    sxml_regex_class *classes;
    sxml_regex_class_item *class_items;
    sxml_regex_counted_loop *counted_loops;
    /* group_count + 1 entries; entry 0 (the whole match) is unnamed. */
    sxml_regex_group *groups;
    /* The names of the named groups, one after the other. */
    uint8_t *group_names;
    /* The allocator of sxml_regex_create; the null allocator otherwise. */
    sxml_allocator owning_allocator;
};

/*
 * What a compiling pass produced. In the result of a pass, the
 * instruction, class, class item and counted loop counts are the sizes of
 * the arrays:
 * the most the pass wrote, since an atom quantified with {0} is written
 * and then dropped.
 */
typedef struct sxml_regex_counts {
    uint32_t instruction_count;
    uint32_t class_count;
    uint32_t class_item_count;
    uint32_t counted_loop_count;
    uint32_t group_count;
    uint32_t register_count;
    uint32_t group_name_byte_count;
} sxml_regex_counts;

/* ------------------------------------------------------------- Compiler */

/*
 * Instructions whose operand is a target not known yet. Each pending
 * operand holds the next pending instruction plus one, so the list needs
 * no memory of its own.
 */
typedef struct sxml_regex_pending_targets {
    uint32_t last_instruction_number;
} sxml_regex_pending_targets;

/* The counts and parse work before an atom, so that its quantifier can
   drop or copy what the atom emitted. */
typedef struct sxml_regex_atom_start {
    sxml_regex_counts counts;
    uint32_t atom_parse_count;
} sxml_regex_atom_start;

/* What a parenthesized construct is (F&O 4.0, section 6.1.3). */
typedef enum sxml_regex_frame_kind {
    /* The whole regex, or a group: capturing, named or (?:...). */
    SXML_REGEX_FRAME_KIND_GROUP,
    /* (?=...) and (*positive_lookahead:...), section 6.1.8.3. */
    SXML_REGEX_FRAME_KIND_POSITIVE_LOOKAHEAD,
    /* (?!...) and (*negative_lookahead:...), section 6.1.8.4. */
    SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKAHEAD,
    /* (?<=...) and (*positive_lookbehind:...), section 6.1.8.5. */
    SXML_REGEX_FRAME_KIND_POSITIVE_LOOKBEHIND,
    /* (?<!...) and (*negative_lookbehind:...), section 6.1.8.6. */
    SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKBEHIND,
} sxml_regex_frame_kind;

/*
 * A group or assertion whose ")" has not been read yet, or the whole regex
 * (the first frame). The parser keeps these on an explicit stack instead
 * of recursing (docs/DECISIONS.md D71).
 */
typedef struct sxml_regex_group_frame {
    sxml_regex_frame_kind kind;
    /* The capturing group's number; 0 for a non-capturing group, an
       assertion and the whole regex. */
    uint32_t group_number;
    /* The first instruction of the branch being read; in a lookbehind
       assertion, its STEP_BACK. */
    uint32_t branch_start;
    /* The jumps at the ends of the branches read so far. */
    sxml_regex_pending_targets branch_ends;
    /* Where the group's atom starts, before its "(". */
    sxml_regex_atom_start atom_start;
    /* An assertion's LOOK_START. */
    uint32_t look_start_index;
    /* A lookbehind assertion: the characters of the branch being read. */
    uint32_t branch_length;
    /* Within a lookahead assertion, this one included. */
    bool is_in_lookahead;
} sxml_regex_group_frame;

typedef struct sxml_regex_compiler {
    sxml_string regex_text;
    size_t byte_offset;
    /* Inside [...], where x and c remove nothing (section 6.2). */
    bool is_in_class;
    sxml_regex_flags flags;
    /* The regex being written; NULL in the counting pass. */
    sxml_regex *regex;
    sxml_regex_counts counts;
    /* The most instructions, classes and class items written so far. */
    sxml_regex_counts array_counts;
    /* Groups of the whole regex, from the counting pass; progress
       registers are numbered after their slots. */
    uint32_t total_group_count;
    uint32_t maximum_instruction_count;
    uint32_t maximum_group_depth;
    /* Atoms parsed, copies included: bounds compile work even for atoms
       that emit nothing, as in (?:){1000}{1000}. */
    uint32_t atom_parse_count;
    /* The stack of open groups; frames[0] is the whole regex, so the
       group depth is frame_count - 1. */
    sxml_regex_group_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
    /* The frames sxml_regex_compile holds itself; frames is these until
       the stack grows. */
    sxml_regex_group_frame *inline_frames;
    /* The names of the named groups read so far, so that duplicates and
       \k<name> are found in the counting pass too: entries of a group
       number and a byte count (two uint32_t, unaligned) and the name's
       bytes. A name being read is appended after the last entry. */
    uint8_t *name_table;
    size_t name_table_byte_count;
    size_t name_table_capacity;
    /* The table sxml_regex_compile holds itself; name_table is this until
       it grows. */
    uint8_t *inline_name_table;
    /* Grows the stack and the name table beyond their inline memory; may
       be the null allocator. */
    sxml_allocator stack_allocator;
    sxml_error_record *error_record;
} sxml_regex_compiler;

static sxml_status sxml_regex_syntax_error(sxml_regex_compiler *compiler,
                                           sxml_byte_view description) {
    return sxml_regex_raise_error(compiler->error_record,
                                  SXML_REGEX_SYNTAX_ERROR, description);
}

#define SXML_REGEX_INVALID(compiler, text)                                   \
    sxml_regex_syntax_error((compiler), SXML_BYTE_VIEW_FROM_LITERAL(          \
                                            "Invalid regular expression: " text))

/* --------------------------------------------------------------- Reading */

/* The whitespace the x flag removes (F&O 4.0, section 6.2). */
static bool sxml_regex_is_removable_whitespace(uint32_t codepoint) {
    return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
           codepoint == '\r';
}

static bool sxml_regex_compiler_peek_raw(const sxml_regex_compiler *compiler,
                                         uint32_t *codepoint) {
    sxml_codepoint_iterator iterator = {.string = compiler->regex_text,
                                        .byte_offset = compiler->byte_offset};
    return sxml_codepoint_iterator_next(&iterator, codepoint);
}

/* The codepoint after the next one, without skipping anything. */
static bool sxml_regex_compiler_peek_second_raw(
    const sxml_regex_compiler *compiler, uint32_t *codepoint) {
    sxml_codepoint_iterator iterator = {.string = compiler->regex_text,
                                        .byte_offset = compiler->byte_offset};
    uint32_t first_codepoint = 0;
    if (!sxml_codepoint_iterator_next(&iterator, &first_codepoint)) {
        return false;
    }
    return sxml_codepoint_iterator_next(&iterator, codepoint);
}

static bool sxml_regex_compiler_next_raw(sxml_regex_compiler *compiler,
                                         uint32_t *codepoint) {
    sxml_codepoint_iterator iterator = {.string = compiler->regex_text,
                                        .byte_offset = compiler->byte_offset};
    if (!sxml_codepoint_iterator_next(&iterator, codepoint)) {
        return false;
    }
    compiler->byte_offset = iterator.byte_offset;
    return true;
}

/* Skips what the x flag removes at the current position. */
static void sxml_regex_compiler_skip_whitespace(
    sxml_regex_compiler *compiler) {
    if (compiler->is_in_class || !compiler->flags.is_whitespace_removed) {
        return;
    }
    uint32_t codepoint = 0;
    while (sxml_regex_compiler_peek_raw(compiler, &codepoint) &&
           sxml_regex_is_removable_whitespace(codepoint)) {
        (void)sxml_regex_compiler_next_raw(compiler, &codepoint);
    }
}

/*
 * Skips what the x and c flags remove at the current position: whitespace
 * and comments outside character class expressions (F&O 4.0, sections
 * 6.1.2 and 6.2). A comment runs to the next "#" or the end.
 */
static void sxml_regex_compiler_skip_ignored(sxml_regex_compiler *compiler) {
    if (compiler->is_in_class) {
        return;
    }
    for (;;) {
        sxml_regex_compiler_skip_whitespace(compiler);
        uint32_t codepoint = 0;
        if (!compiler->flags.has_comments ||
            !sxml_regex_compiler_peek_raw(compiler, &codepoint) ||
            codepoint != '#') {
            return;
        }
        (void)sxml_regex_compiler_next_raw(compiler, &codepoint);
        while (sxml_regex_compiler_next_raw(compiler, &codepoint) &&
               codepoint != '#') {
        }
    }
}

static bool sxml_regex_compiler_peek(sxml_regex_compiler *compiler,
                                     uint32_t *codepoint) {
    sxml_regex_compiler_skip_ignored(compiler);
    return sxml_regex_compiler_peek_raw(compiler, codepoint);
}

static bool sxml_regex_compiler_next(sxml_regex_compiler *compiler,
                                     uint32_t *codepoint) {
    sxml_regex_compiler_skip_ignored(compiler);
    return sxml_regex_compiler_next_raw(compiler, codepoint);
}

/*
 * Reads the codepoint after a backslash. Whitespace is removed before
 * parsing, so with x "\ s" is "\s"; a "#" after a backslash is escaped,
 * never a comment (section 6.1.2).
 */
static bool sxml_regex_compiler_next_escaped(sxml_regex_compiler *compiler,
                                             uint32_t *codepoint) {
    sxml_regex_compiler_skip_whitespace(compiler);
    return sxml_regex_compiler_next_raw(compiler, codepoint);
}

/* -------------------------------------------------------------- Emitting */

static sxml_status sxml_regex_compiler_emit(
    sxml_regex_compiler *compiler, sxml_regex_instruction instruction,
    uint32_t *instruction_index) {
    if (compiler->counts.instruction_count >=
        compiler->maximum_instruction_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    uint32_t new_index = compiler->counts.instruction_count;
    compiler->counts.instruction_count = new_index + 1;
    if (compiler->regex != NULL) {
        compiler->regex->instructions[new_index] = instruction;
    }
    *instruction_index = new_index;
    return SXML_STATUS_OK;
}

static sxml_status sxml_regex_compiler_emit_operation(
    sxml_regex_compiler *compiler, sxml_regex_operation operation,
    uint32_t operand) {
    uint32_t instruction_index = 0;
    return sxml_regex_compiler_emit(
        compiler,
        (sxml_regex_instruction){.operation = operation, .operand = operand},
        &instruction_index);
}

/* Sets the operand of an instruction emitted earlier (a forward target). */
static void sxml_regex_compiler_patch(sxml_regex_compiler *compiler,
                                      uint32_t instruction_index,
                                      uint32_t operand) {
    if (compiler->regex != NULL) {
        compiler->regex->instructions[instruction_index].operand = operand;
    }
}

static void sxml_regex_pending_targets_add(
    sxml_regex_compiler *compiler, sxml_regex_pending_targets *pending,
    uint32_t instruction_index) {
    sxml_regex_compiler_patch(compiler, instruction_index,
                              pending->last_instruction_number);
    pending->last_instruction_number = instruction_index + 1;
}

static void sxml_regex_pending_targets_resolve(
    sxml_regex_compiler *compiler, sxml_regex_pending_targets pending,
    uint32_t target) {
    if (compiler->regex == NULL) {
        return;
    }
    uint32_t instruction_number = pending.last_instruction_number;
    while (instruction_number != 0) {
        sxml_regex_instruction *instruction =
            &compiler->regex->instructions[instruction_number - 1];
        instruction_number = instruction->operand;
        instruction->operand = target;
    }
}

/* Writes an instruction into a place made by sxml_regex_compiler_insert. */
static void sxml_regex_compiler_write(sxml_regex_compiler *compiler,
                                      uint32_t instruction_index,
                                      sxml_regex_instruction instruction) {
    if (compiler->regex != NULL) {
        compiler->regex->instructions[instruction_index] = instruction;
    }
}

/* True for the operations whose operand is an instruction index. */
static bool sxml_regex_operation_has_target(sxml_regex_operation operation) {
    return operation == SXML_REGEX_OPERATION_SPLIT_NEXT_FIRST ||
           operation == SXML_REGEX_OPERATION_SPLIT_TARGET_FIRST ||
           operation == SXML_REGEX_OPERATION_JUMP ||
           operation == SXML_REGEX_OPERATION_COUNTED_LOOP_END ||
           operation == SXML_REGEX_OPERATION_POSITIVE_LOOK_START ||
           operation == SXML_REGEX_OPERATION_NEGATIVE_LOOK_START;
}

/* Raises the array counts to what has been written so far. */
static void sxml_regex_compiler_note_array_counts(
    sxml_regex_compiler *compiler) {
    sxml_regex_counts *array_counts = &compiler->array_counts;
    if (compiler->counts.instruction_count > array_counts->instruction_count) {
        array_counts->instruction_count = compiler->counts.instruction_count;
    }
    if (compiler->counts.class_count > array_counts->class_count) {
        array_counts->class_count = compiler->counts.class_count;
    }
    if (compiler->counts.class_item_count > array_counts->class_item_count) {
        array_counts->class_item_count = compiler->counts.class_item_count;
    }
    if (compiler->counts.counted_loop_count >
        array_counts->counted_loop_count) {
        array_counts->counted_loop_count = compiler->counts.counted_loop_count;
    }
}

/* A run of instructions. */
typedef struct sxml_regex_instruction_range {
    uint32_t first_instruction_index;
    uint32_t instruction_count;
} sxml_regex_instruction_range;

/*
 * Makes room for range.instruction_count instructions at
 * range.first_instruction_index by moving the instructions from there on
 * up. Targets of the moved instructions move with them. The moved
 * instructions are one finished branch or atom, whose targets all lie
 * within it; targets of earlier instructions stay, so a target at the
 * place now reaches the new instructions, which belong to the same branch
 * or piece. No pending target list runs through the moved instructions.
 */
static sxml_status sxml_regex_compiler_insert(
    sxml_regex_compiler *compiler, sxml_regex_instruction_range range) {
    uint32_t instruction_count = compiler->counts.instruction_count;
    if (range.instruction_count >
        compiler->maximum_instruction_count - instruction_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (compiler->regex != NULL) {
        sxml_regex_instruction *instructions = compiler->regex->instructions;
        uint32_t moved_count =
            instruction_count - range.first_instruction_index;
        uint32_t new_first_index =
            range.first_instruction_index + range.instruction_count;
        memmove(instructions + new_first_index,
                instructions + range.first_instruction_index,
                (size_t)moved_count * sizeof(sxml_regex_instruction));
        for (uint32_t instruction_index = new_first_index;
             instruction_index < new_first_index + moved_count;
             instruction_index++) {
            sxml_regex_instruction *instruction =
                &instructions[instruction_index];
            if (sxml_regex_operation_has_target(instruction->operation) &&
                instruction->operand >= range.first_instruction_index) {
                instruction->operand += range.instruction_count;
            }
        }
    }
    compiler->counts.instruction_count =
        instruction_count + range.instruction_count;
    return SXML_STATUS_OK;
}

/* The slot of a new progress register (numbered after the group slots). */
static uint32_t sxml_regex_compiler_add_register(
    sxml_regex_compiler *compiler) {
    uint32_t progress_slot = 2 * (compiler->total_group_count + 1) +
                             compiler->counts.register_count;
    compiler->counts.register_count += 1;
    return progress_slot;
}

/* What an atom emitted once, for the copies its quantifier needs. */
typedef struct sxml_regex_atom_copy_source {
    sxml_regex_instruction_range instructions;
    uint32_t first_register_number;
    uint32_t register_count;
    /* The atoms parsing it took, which every copy counts again. */
    uint32_t atom_parse_count;
} sxml_regex_atom_copy_source;

/*
 * Appends a copy of an atom's instructions. Targets are moved with the
 * copy (they all lie within the atom or at its end), progress registers
 * are renumbered so that every copy has its own, and group slots stay:
 * every copy numbers its groups alike. Classes are shared.
 */
static sxml_status sxml_regex_compiler_emit_copy(
    sxml_regex_compiler *compiler, const sxml_regex_atom_copy_source *source) {
    if (source->atom_parse_count >
        compiler->maximum_instruction_count - compiler->atom_parse_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    compiler->atom_parse_count += source->atom_parse_count;
    uint32_t new_first_index = compiler->counts.instruction_count;
    if (source->instructions.instruction_count >
        compiler->maximum_instruction_count - new_first_index) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (compiler->regex != NULL) {
        sxml_regex_instruction *instructions = compiler->regex->instructions;
        uint32_t first_register_slot = 2 * (compiler->total_group_count + 1);
        uint32_t register_offset =
            compiler->counts.register_count - source->first_register_number;
        uint32_t target_offset =
            new_first_index - source->instructions.first_instruction_index;
        for (uint32_t copied_count = 0;
             copied_count < source->instructions.instruction_count;
             copied_count++) {
            sxml_regex_instruction instruction =
                instructions[source->instructions.first_instruction_index +
                             copied_count];
            if (sxml_regex_operation_has_target(instruction.operation)) {
                instruction.operand += target_offset;
            } else if ((instruction.operation == SXML_REGEX_OPERATION_SAVE ||
                        instruction.operation ==
                            SXML_REGEX_OPERATION_CHECK_PROGRESS) &&
                       instruction.operand >= first_register_slot) {
                instruction.operand += register_offset;
            }
            instructions[new_first_index + copied_count] = instruction;
        }
    }
    compiler->counts.instruction_count =
        new_first_index + source->instructions.instruction_count;
    compiler->counts.register_count += source->register_count;
    return SXML_STATUS_OK;
}

static sxml_status sxml_regex_compiler_reserve_class(
    sxml_regex_compiler *compiler, uint32_t *class_index) {
    if (compiler->counts.class_count >= compiler->maximum_instruction_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    *class_index = compiler->counts.class_count;
    compiler->counts.class_count += 1;
    return SXML_STATUS_OK;
}

static void sxml_regex_compiler_write_class(sxml_regex_compiler *compiler,
                                            uint32_t class_index,
                                            sxml_regex_class regex_class) {
    if (compiler->regex != NULL) {
        compiler->regex->classes[class_index] = regex_class;
    }
}

static sxml_status sxml_regex_compiler_emit_class_item(
    sxml_regex_compiler *compiler, sxml_regex_class_item item) {
    if (compiler->counts.class_item_count >=
        compiler->maximum_instruction_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    uint32_t item_index = compiler->counts.class_item_count;
    compiler->counts.class_item_count = item_index + 1;
    if (compiler->regex != NULL) {
        compiler->regex->class_items[item_index] = item;
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_regex_compiler_emit_range_item(
    sxml_regex_compiler *compiler, sxml_regex_codepoint_range range) {
    return sxml_regex_compiler_emit_class_item(
        compiler, (sxml_regex_class_item){
                      .kind = SXML_REGEX_CLASS_ITEM_KIND_RANGE,
                      .is_complemented = false,
                      .is_case_insensitive = false,
                      .first_codepoint = range.first_codepoint,
                      .last_codepoint = range.last_codepoint,
                      .category_mask = 0,
                  });
}

/*
 * Emits a character or range of a character group. Under the i flag it
 * also matches the case variants of its characters (F&O 4.0, section 6.2:
 * C2 is a case variant of C1 when their fn:lower-case or their
 * fn:upper-case are equal), which the matcher checks with the Unicode case
 * mappings, since a range may hold any number of characters.
 */
static sxml_status sxml_regex_compiler_emit_range(
    sxml_regex_compiler *compiler, sxml_regex_codepoint_range range) {
    return sxml_regex_compiler_emit_class_item(
        compiler, (sxml_regex_class_item){
                      .kind = SXML_REGEX_CLASS_ITEM_KIND_RANGE,
                      .is_complemented = false,
                      .is_case_insensitive = compiler->flags.is_case_insensitive,
                      .first_codepoint = range.first_codepoint,
                      .last_codepoint = range.last_codepoint,
                      .category_mask = 0,
                  });
}

/* Emits the instruction that matches class class_index. */
static sxml_status sxml_regex_compiler_emit_class_instruction(
    sxml_regex_compiler *compiler, uint32_t class_index) {
    return sxml_regex_compiler_emit_operation(
        compiler, SXML_REGEX_OPERATION_CLASS, class_index);
}

/*
 * Emits a normal character as an atom. Under the i flag a character with
 * case variants becomes a class of the character and its variants, found
 * once here (section 6.2).
 */
static sxml_status sxml_regex_compiler_emit_literal(
    sxml_regex_compiler *compiler, uint32_t codepoint) {
    sxml_regex_case_variants variants = {.codepoint_count = 1};
    variants.codepoints[0] = codepoint;
    if (compiler->flags.is_case_insensitive) {
        sxml_regex_find_case_variants(codepoint, &variants);
    }
    if (variants.codepoint_count == 1) {
        return sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_CODEPOINT, codepoint);
    }
    uint32_t class_index = 0;
    sxml_status status =
        sxml_regex_compiler_reserve_class(compiler, &class_index);
    uint32_t first_item_index = compiler->counts.class_item_count;
    for (size_t variant_index = 0;
         status == SXML_STATUS_OK && variant_index < variants.codepoint_count;
         variant_index++) {
        status = sxml_regex_compiler_emit_range_item(
            compiler, (sxml_regex_codepoint_range){
                          .first_codepoint = variants.codepoints[variant_index],
                          .last_codepoint = variants.codepoints[variant_index],
                      });
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_compiler_write_class(
        compiler, class_index,
        (sxml_regex_class){
            .first_item_index = first_item_index,
            .item_count = compiler->counts.class_item_count - first_item_index,
            .is_negated = false,
            .subtracted_class_number = 0,
        });
    return sxml_regex_compiler_emit_class_instruction(compiler, class_index);
}

/* ---------------------------------------------------- Escapes and classes */

/*
 * The character of a single character escape (F&O 4.0, section 6.1.3,
 * SingleCharEsc: \n \r \t and the escaped metacharacters, "$" and "#"
 * included), or false when escaped_codepoint is not one.
 */
static bool sxml_regex_single_character_escape(uint32_t escaped_codepoint,
                                               uint32_t *codepoint) {
    switch (escaped_codepoint) {
    case 'n':
        *codepoint = '\n';
        return true;
    case 'r':
        *codepoint = '\r';
        return true;
    case 't':
        *codepoint = '\t';
        return true;
    case '\\':
    case '|':
    case '.':
    case '?':
    case '*':
    case '+':
    case '(':
    case ')':
    case '{':
    case '}':
    case '$':
    case '-':
    case '[':
    case ']':
    case '^':
    case '#':
        *codepoint = escaped_codepoint;
        return true;
    default:
        return false;
    }
}

/* The names of category escapes (XSD 1.1 Part 2, appendix G, "Category
   escapes": IsCategory) and the categories each stands for. */
typedef struct sxml_regex_category_name {
    char name[3];
    uint32_t category_mask;
} sxml_regex_category_name;

#define SXML_REGEX_CATEGORY_BIT(category)                                    \
    ((uint32_t)1 << (uint32_t)(SXML_REGEX_GENERAL_CATEGORY_##category))

static const sxml_regex_category_name sxml_regex_category_names[] = {
    {"L", SXML_REGEX_CATEGORY_MASK_L},  {"Lu", SXML_REGEX_CATEGORY_BIT(LU)},
    {"Ll", SXML_REGEX_CATEGORY_BIT(LL)}, {"Lt", SXML_REGEX_CATEGORY_BIT(LT)},
    {"Lm", SXML_REGEX_CATEGORY_BIT(LM)}, {"Lo", SXML_REGEX_CATEGORY_BIT(LO)},
    {"M", SXML_REGEX_CATEGORY_MASK_M},  {"Mn", SXML_REGEX_CATEGORY_BIT(MN)},
    {"Mc", SXML_REGEX_CATEGORY_BIT(MC)}, {"Me", SXML_REGEX_CATEGORY_BIT(ME)},
    {"N", SXML_REGEX_CATEGORY_MASK_N},  {"Nd", SXML_REGEX_CATEGORY_BIT(ND)},
    {"Nl", SXML_REGEX_CATEGORY_BIT(NL)}, {"No", SXML_REGEX_CATEGORY_BIT(NO)},
    {"P", SXML_REGEX_CATEGORY_MASK_P},  {"Pc", SXML_REGEX_CATEGORY_BIT(PC)},
    {"Pd", SXML_REGEX_CATEGORY_BIT(PD)}, {"Ps", SXML_REGEX_CATEGORY_BIT(PS)},
    {"Pe", SXML_REGEX_CATEGORY_BIT(PE)}, {"Pi", SXML_REGEX_CATEGORY_BIT(PI)},
    {"Pf", SXML_REGEX_CATEGORY_BIT(PF)}, {"Po", SXML_REGEX_CATEGORY_BIT(PO)},
    {"Z", SXML_REGEX_CATEGORY_MASK_Z},  {"Zs", SXML_REGEX_CATEGORY_BIT(ZS)},
    {"Zl", SXML_REGEX_CATEGORY_BIT(ZL)}, {"Zp", SXML_REGEX_CATEGORY_BIT(ZP)},
    {"S", SXML_REGEX_CATEGORY_MASK_S},  {"Sm", SXML_REGEX_CATEGORY_BIT(SM)},
    {"Sc", SXML_REGEX_CATEGORY_BIT(SC)}, {"Sk", SXML_REGEX_CATEGORY_BIT(SK)},
    {"So", SXML_REGEX_CATEGORY_BIT(SO)}, {"C", SXML_REGEX_CATEGORY_MASK_C},
    {"Cc", SXML_REGEX_CATEGORY_BIT(CC)}, {"Cf", SXML_REGEX_CATEGORY_BIT(CF)},
    {"Co", SXML_REGEX_CATEGORY_BIT(CO)}, {"Cn", SXML_REGEX_CATEGORY_BIT(CN)},
};

/* The longest charProp read: "Is" and the longest block name of Unicode
   16.0 (44 bytes) fit with room to spare. */
#define SXML_REGEX_MAXIMUM_PROPERTY_BYTE_COUNT 64U

/* True for the characters of a block name (IsBlock: [a-zA-Z0-9-]). */
static bool sxml_regex_is_block_name_character(uint32_t codepoint) {
    return (codepoint >= 'a' && codepoint <= 'z') ||
           (codepoint >= 'A' && codepoint <= 'Z') ||
           (codepoint >= '0' && codepoint <= '9') || codepoint == '-';
}

/*
 * Reads "{" charProp "}" after "\p" or "\P" (XSD 1.1 Part 2, appendix G,
 * catEsc and complEsc) into the item of a category or block escape. An
 * unknown block is FORX0002 (F&O 4.0, section 6.1.7). Inside a class
 * expression nothing is removed; outside, the x flag has removed
 * whitespace, so "\p{ Lu }" is "\p{Lu}".
 */
static sxml_status sxml_regex_compiler_parse_property(
    sxml_regex_compiler *compiler, sxml_regex_class_item *item) {
    uint32_t codepoint = 0;
    if (!sxml_regex_compiler_next(compiler, &codepoint) || codepoint != '{') {
        return SXML_REGEX_INVALID(compiler, "\"\\p\" must be followed by \"{\"");
    }
    uint8_t property[SXML_REGEX_MAXIMUM_PROPERTY_BYTE_COUNT];
    size_t property_byte_count = 0;
    for (;;) {
        if (!sxml_regex_compiler_next(compiler, &codepoint)) {
            return SXML_REGEX_INVALID(compiler, "\"\\p{\" without \"}\"");
        }
        if (codepoint == '}') {
            break;
        }
        if (!sxml_regex_is_block_name_character(codepoint) ||
            property_byte_count == SXML_REGEX_MAXIMUM_PROPERTY_BYTE_COUNT) {
            return SXML_REGEX_INVALID(compiler,
                                      "unknown category or block name");
        }
        property[property_byte_count] = (uint8_t)codepoint;
        property_byte_count += 1;
    }
    if (property_byte_count > 2 && property[0] == 'I' && property[1] == 's') {
        sxml_regex_codepoint_range range = {.first_codepoint = 0};
        if (!sxml_regex_find_block(
                (sxml_byte_view){.bytes = property + 2,
                                 .byte_count = property_byte_count - 2},
                &range)) {
            if (!compiler->flags.is_xsd_pattern) {
                return SXML_REGEX_INVALID(compiler, "unknown block name");
            }
            /* XSD 1.1 Part 2, G.4.2.4: \p{IsX} and \P{IsX} of an unknown
               block X both denote all characters. */
            range = (sxml_regex_codepoint_range){.first_codepoint = 0,
                                                 .last_codepoint = 0x10FFFFU};
            item->is_complemented = false;
        }
        item->kind = SXML_REGEX_CLASS_ITEM_KIND_RANGE;
        item->first_codepoint = range.first_codepoint;
        item->last_codepoint = range.last_codepoint;
        return SXML_STATUS_OK;
    }
    for (size_t name_index = 0;
         name_index < sizeof(sxml_regex_category_names) /
                          sizeof(sxml_regex_category_names[0]);
         name_index++) {
        const sxml_regex_category_name *name =
            &sxml_regex_category_names[name_index];
        size_t name_byte_count = name->name[1] == '\0' ? 1 : 2;
        if (name_byte_count == property_byte_count &&
            memcmp(name->name, property, name_byte_count) == 0) {
            item->kind = SXML_REGEX_CLASS_ITEM_KIND_CATEGORY;
            item->category_mask = name->category_mask;
            return SXML_STATUS_OK;
        }
    }
    return SXML_REGEX_INVALID(compiler, "unknown category name");
}

/* What sxml_regex_compiler_parse_class_escape read. */
typedef struct sxml_regex_class_escape {
    /* False when the escaped character starts no class escape; nothing
       was read then. */
    bool is_class_escape;
    sxml_regex_class_item item;
} sxml_regex_class_escape;

/*
 * Reads a class escape (XSD 1.1 Part 2, appendix G, charClassEsc) whose
 * backslash and escaped_codepoint have been read: a multi-character
 * escape (\s \S \i \I \c \C \d \D \w \W) or a category or block escape
 * (\p{..} \P{..}). None of them is affected by the i flag (F&O 4.0,
 * section 6.2).
 */
static sxml_status sxml_regex_compiler_parse_class_escape(
    sxml_regex_compiler *compiler, uint32_t escaped_codepoint,
    sxml_regex_class_escape *escape) {
    sxml_regex_class_item item = {
        .kind = SXML_REGEX_CLASS_ITEM_KIND_SPACE,
        .is_complemented = escaped_codepoint >= 'A' && escaped_codepoint <= 'Z',
        .is_case_insensitive = false,
        .first_codepoint = 0,
        .last_codepoint = 0,
        .category_mask = 0,
    };
    escape->is_class_escape = true;
    switch (escaped_codepoint) {
    case 's':
    case 'S':
        item.kind = SXML_REGEX_CLASS_ITEM_KIND_SPACE;
        break;
    case 'd':
    case 'D':
        item.kind = SXML_REGEX_CLASS_ITEM_KIND_DECIMAL_DIGIT;
        break;
    case 'w':
    case 'W':
        item.kind = SXML_REGEX_CLASS_ITEM_KIND_WORD_CHARACTER;
        break;
    case 'i':
    case 'I':
        item.kind = SXML_REGEX_CLASS_ITEM_KIND_NAME_START_CHARACTER;
        break;
    case 'c':
    case 'C':
        item.kind = SXML_REGEX_CLASS_ITEM_KIND_NAME_CHARACTER;
        break;
    case 'p':
    case 'P': {
        sxml_status status = sxml_regex_compiler_parse_property(compiler, &item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        break;
    }
    default:
        escape->is_class_escape = false;
        return SXML_STATUS_OK;
    }
    escape->item = item;
    return SXML_STATUS_OK;
}

/* Reads the end of a range after "-": a single character, escaped or not
   (XSD 1.1 Part 2, appendix G, charRange). */
static sxml_status sxml_regex_compiler_parse_range_end(
    sxml_regex_compiler *compiler, uint32_t *codepoint) {
    uint32_t read_codepoint = 0;
    if (!sxml_regex_compiler_next_raw(compiler, &read_codepoint)) {
        return SXML_REGEX_INVALID(compiler, "unterminated character class");
    }
    if (read_codepoint == '\\') {
        uint32_t escaped_codepoint = 0;
        if (!sxml_regex_compiler_next_raw(compiler, &escaped_codepoint) ||
            !sxml_regex_single_character_escape(escaped_codepoint,
                                                &read_codepoint)) {
            return SXML_REGEX_INVALID(
                compiler, "a range must end with a single character");
        }
    } else if (read_codepoint == '[' || read_codepoint == '-') {
        return SXML_REGEX_INVALID(compiler,
                                  "\"[\" or \"-\" must be escaped here");
    }
    *codepoint = read_codepoint;
    return SXML_STATUS_OK;
}

/* True when the next two characters are "-" and a character that ends a
   range (not "]" or "["): the hyphen is a range separator (XSD 1.1 Part
   2, appendix G, charRange). */
static bool sxml_regex_compiler_is_at_range_separator(
    const sxml_regex_compiler *compiler) {
    uint32_t following_codepoint = 0;
    uint32_t second_codepoint = 0;
    return sxml_regex_compiler_peek_raw(compiler, &following_codepoint) &&
           following_codepoint == '-' &&
           sxml_regex_compiler_peek_second_raw(compiler, &second_codepoint) &&
           second_codepoint != ']' && second_codepoint != '[';
}

/* Reads a character group part that starts with codepoint (already read),
   and emits its items. */
static sxml_status sxml_regex_compiler_parse_group_part(
    sxml_regex_compiler *compiler, uint32_t codepoint) {
    uint32_t first_codepoint = codepoint;
    if (codepoint == '[') {
        return SXML_REGEX_INVALID(compiler,
                                  "\"[\" must be escaped in a character class");
    }
    if (codepoint == '\\') {
        uint32_t escaped_codepoint = 0;
        if (!sxml_regex_compiler_next_raw(compiler, &escaped_codepoint)) {
            return SXML_REGEX_INVALID(compiler, "unterminated character class");
        }
        sxml_regex_class_escape escape = {.is_class_escape = false};
        sxml_status status = sxml_regex_compiler_parse_class_escape(
            compiler, escaped_codepoint, &escape);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (escape.is_class_escape) {
            if (sxml_regex_compiler_is_at_range_separator(compiler)) {
                return SXML_REGEX_INVALID(
                    compiler, "a range cannot start with a class escape");
            }
            return sxml_regex_compiler_emit_class_item(compiler, escape.item);
        }
        if (!sxml_regex_single_character_escape(escaped_codepoint,
                                                &first_codepoint)) {
            return SXML_REGEX_INVALID(compiler,
                                      "unknown escape in a character class");
        }
    }
    uint32_t last_codepoint = first_codepoint;
    if (sxml_regex_compiler_is_at_range_separator(compiler)) {
        uint32_t hyphen = 0;
        (void)sxml_regex_compiler_next_raw(compiler, &hyphen);
        sxml_status status =
            sxml_regex_compiler_parse_range_end(compiler, &last_codepoint);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (last_codepoint < first_codepoint) {
            return SXML_REGEX_INVALID(compiler,
                                      "a range ends before it starts");
        }
    }
    return sxml_regex_compiler_emit_range(
        compiler, (sxml_regex_codepoint_range){
                      .first_codepoint = first_codepoint,
                      .last_codepoint = last_codepoint,
                  });
}

/* The nesting of groups and class expressions the compiler is in. */
static size_t sxml_regex_compiler_group_depth(
    const sxml_regex_compiler *compiler) {
    return compiler->frame_count - 1;
}

/* Starts one class expression of a subtraction chain after its "[":
   reserves its class and reads an optional "^". */
static sxml_status sxml_regex_compiler_open_class(
    sxml_regex_compiler *compiler, size_t class_depth,
    sxml_regex_class *regex_class) {
    if (sxml_regex_compiler_group_depth(compiler) + class_depth >=
        compiler->maximum_group_depth) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    uint32_t class_index = 0;
    sxml_status status =
        sxml_regex_compiler_reserve_class(compiler, &class_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *regex_class = (sxml_regex_class){
        .first_item_index = compiler->counts.class_item_count,
        .item_count = 0,
        .is_negated = false,
        .subtracted_class_number = 0,
    };
    uint32_t codepoint = 0;
    if (sxml_regex_compiler_peek_raw(compiler, &codepoint) && codepoint == '^') {
        (void)sxml_regex_compiler_next_raw(compiler, &codepoint);
        regex_class->is_negated = true;
    }
    return SXML_STATUS_OK;
}

/* How one class expression of a subtraction chain ended. */
typedef struct sxml_regex_class_end {
    /* "-[" was read: the subtracted class expression follows. */
    bool has_subtraction;
} sxml_regex_class_end;

/*
 * Reads the group parts of one class expression up to its "]" or its
 * subtraction "-[" (XSD 1.1 Part 2, appendix G, charClassExpr). A "-"
 * that separates no range is a character (XSD 1.1 allows it anywhere,
 * QT4 re00056a: "[a-d-b-c]" is a-d, "-" and b-c), except before "[".
 */
static sxml_status sxml_regex_compiler_read_class_parts(
    sxml_regex_compiler *compiler, sxml_regex_class_end *class_end) {
    uint32_t part_count = 0;
    for (;;) {
        uint32_t codepoint = 0;
        if (!sxml_regex_compiler_next_raw(compiler, &codepoint)) {
            return SXML_REGEX_INVALID(compiler, "unterminated character class");
        }
        if (codepoint == ']') {
            if (part_count == 0) {
                return SXML_REGEX_INVALID(compiler, "empty character class");
            }
            class_end->has_subtraction = false;
            return SXML_STATUS_OK;
        }
        uint32_t following_codepoint = 0;
        bool has_following = sxml_regex_compiler_peek_raw(compiler,
                                                          &following_codepoint);
        if (codepoint == '-' && has_following && following_codepoint == '[') {
            if (part_count == 0) {
                return SXML_REGEX_INVALID(compiler,
                                          "subtraction from an empty class");
            }
            (void)sxml_regex_compiler_next_raw(compiler, &following_codepoint);
            class_end->has_subtraction = true;
            return SXML_STATUS_OK;
        }
        sxml_status status =
            sxml_regex_compiler_parse_group_part(compiler, codepoint);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        part_count += 1;
    }
}

/*
 * Reads a character class expression after its "[" and writes the index
 * of its class to *class_index. A subtraction ([a-z-[aeiou]]) ends its
 * class expression, so nested subtractions form a chain: each class
 * expression is written when its "-[" is read, with the class reserved
 * next as its subtracted class, and the "]" of the enclosing expressions
 * are read after the innermost one. The chain is bounded by
 * maximum_group_depth together with the enclosing groups.
 */
static sxml_status sxml_regex_compiler_parse_class_expression(
    sxml_regex_compiler *compiler, uint32_t *class_index) {
    bool was_in_class = compiler->is_in_class;
    compiler->is_in_class = true;
    uint32_t first_class_index = compiler->counts.class_count;
    size_t class_depth = 0;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        uint32_t new_class_index = compiler->counts.class_count;
        sxml_regex_class regex_class = {.first_item_index = 0};
        status = sxml_regex_compiler_open_class(compiler, class_depth,
                                                &regex_class);
        if (status != SXML_STATUS_OK) {
            break;
        }
        class_depth += 1;
        sxml_regex_class_end class_end = {.has_subtraction = false};
        status = sxml_regex_compiler_read_class_parts(compiler, &class_end);
        if (status != SXML_STATUS_OK) {
            break;
        }
        regex_class.item_count =
            compiler->counts.class_item_count - regex_class.first_item_index;
        if (class_end.has_subtraction) {
            /* The subtracted class is the one reserved next. */
            regex_class.subtracted_class_number =
                compiler->counts.class_count + 1;
        }
        sxml_regex_compiler_write_class(compiler, new_class_index, regex_class);
        if (!class_end.has_subtraction) {
            break;
        }
    }
    /* The "]" after each subtraction, innermost first. */
    for (size_t closed_count = 1;
         status == SXML_STATUS_OK && closed_count < class_depth;
         closed_count++) {
        uint32_t codepoint = 0;
        if (!sxml_regex_compiler_next_raw(compiler, &codepoint) ||
            codepoint != ']') {
            status = SXML_REGEX_INVALID(
                compiler, "a subtraction must end the character class");
        }
    }
    compiler->is_in_class = was_in_class;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *class_index = first_class_index;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Group stack */

/* Doubles the group stack, up to the frames maximum_group_depth needs. */
static sxml_status sxml_regex_compiler_grow_frames(
    sxml_regex_compiler *compiler) {
    if (sxml_allocator_is_null(compiler->stack_allocator)) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    size_t maximum_frame_count = (size_t)compiler->maximum_group_depth + 1;
    size_t new_capacity = compiler->frame_capacity;
    if (sxml_size_multiply(new_capacity, 2, &new_capacity) != SXML_STATUS_OK ||
        new_capacity > maximum_frame_count) {
        new_capacity = maximum_frame_count;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        new_capacity, sizeof(sxml_regex_group_frame), &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        compiler->stack_allocator,
        (sxml_allocation_request){
            .byte_count = byte_count,
            .alignment = alignof(sxml_regex_group_frame),
        },
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_group_frame *new_frames = memory;
    memcpy(new_frames, compiler->frames,
           compiler->frame_count * sizeof(sxml_regex_group_frame));
    if (compiler->frames != compiler->inline_frames) {
        sxml_allocator_release(compiler->stack_allocator, compiler->frames);
    }
    compiler->frames = new_frames;
    compiler->frame_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* Gives a grown group stack back to its allocator. */
static void sxml_regex_compiler_release_frames(sxml_regex_compiler *compiler) {
    if (compiler->frames != compiler->inline_frames) {
        sxml_allocator_release(compiler->stack_allocator, compiler->frames);
        compiler->frames = compiler->inline_frames;
    }
}

/* Pushes frame; the caller has checked maximum_group_depth. */
static sxml_status sxml_regex_compiler_push_frame(
    sxml_regex_compiler *compiler, sxml_regex_group_frame frame) {
    if (compiler->frame_count == compiler->frame_capacity) {
        sxml_status status = sxml_regex_compiler_grow_frames(compiler);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    compiler->frames[compiler->frame_count] = frame;
    compiler->frame_count += 1;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Group names */

/* The bytes before a name in the name table: its group number and byte
   count. */
#define SXML_REGEX_NAME_ENTRY_HEADER_BYTE_COUNT (2U * sizeof(uint32_t))
/* The name table sxml_regex_compile holds itself. */
#define SXML_REGEX_INLINE_NAME_TABLE_BYTE_COUNT 256U

/* Makes room for byte_count more bytes in the name table. */
static sxml_status sxml_regex_compiler_reserve_names(
    sxml_regex_compiler *compiler, size_t byte_count) {
    if (byte_count <= compiler->name_table_capacity -
                          compiler->name_table_byte_count) {
        return SXML_STATUS_OK;
    }
    if (sxml_allocator_is_null(compiler->stack_allocator)) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    size_t needed_byte_count = 0;
    sxml_status status = sxml_size_add(compiler->name_table_byte_count,
                                       byte_count, &needed_byte_count);
    size_t new_capacity = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(compiler->name_table_capacity, 2,
                                    &new_capacity);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (new_capacity < needed_byte_count) {
        new_capacity = needed_byte_count;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        compiler->stack_allocator,
        (sxml_allocation_request){.byte_count = new_capacity, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *new_table = memory;
    memcpy(new_table, compiler->name_table, compiler->name_table_byte_count);
    if (compiler->name_table != compiler->inline_name_table) {
        sxml_allocator_release(compiler->stack_allocator, compiler->name_table);
    }
    compiler->name_table = new_table;
    compiler->name_table_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* Gives a grown name table back to its allocator. */
static void sxml_regex_compiler_release_names(sxml_regex_compiler *compiler) {
    if (compiler->name_table != compiler->inline_name_table) {
        sxml_allocator_release(compiler->stack_allocator, compiler->name_table);
        compiler->name_table = compiler->inline_name_table;
    }
}

/* The uint32_t at byte_offset of the name table (entries are not
   aligned). */
static uint32_t sxml_regex_compiler_read_name_table_field(
    const sxml_regex_compiler *compiler, size_t byte_offset) {
    uint32_t value = 0;
    memcpy(&value, compiler->name_table + byte_offset, sizeof(value));
    return value;
}

/* A group name just read: it lies after the last entry of the name
   table, from byte_offset, until the next append. */
typedef struct sxml_regex_read_name {
    size_t byte_offset;
    size_t byte_count;
} sxml_regex_read_name;

static sxml_byte_view sxml_regex_compiler_name_view(
    const sxml_regex_compiler *compiler, sxml_regex_read_name name) {
    return (sxml_byte_view){.bytes = compiler->name_table + name.byte_offset,
                            .byte_count = name.byte_count};
}

/*
 * Reads a group name up to its ">" (F&O 4.0, section 6.1.3, groupName: an
 * ASCII letter, then ASCII letters and digits) after the end of the name
 * table, leaving room for an entry header before it.
 */
static sxml_status sxml_regex_compiler_read_group_name(
    sxml_regex_compiler *compiler, sxml_regex_read_name *name) {
    sxml_status status = sxml_regex_compiler_reserve_names(
        compiler, SXML_REGEX_NAME_ENTRY_HEADER_BYTE_COUNT);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t name_offset =
        compiler->name_table_byte_count + SXML_REGEX_NAME_ENTRY_HEADER_BYTE_COUNT;
    size_t name_byte_count = 0;
    for (;;) {
        uint32_t codepoint = 0;
        if (!sxml_regex_compiler_next(compiler, &codepoint)) {
            return SXML_REGEX_INVALID(compiler, "a group name must end with \">\"");
        }
        if (codepoint == '>') {
            break;
        }
        bool is_letter = (codepoint >= 'a' && codepoint <= 'z') ||
                         (codepoint >= 'A' && codepoint <= 'Z');
        bool is_digit = codepoint >= '0' && codepoint <= '9';
        if (!is_letter && !(is_digit && name_byte_count != 0)) {
            return SXML_REGEX_INVALID(
                compiler, "a group name is an ASCII letter followed by ASCII "
                          "letters and digits");
        }
        status = sxml_regex_compiler_reserve_names(
            compiler, SXML_REGEX_NAME_ENTRY_HEADER_BYTE_COUNT +
                          name_byte_count + 1);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        compiler->name_table[name_offset + name_byte_count] = (uint8_t)codepoint;
        name_byte_count += 1;
    }
    if (name_byte_count == 0) {
        return SXML_REGEX_INVALID(compiler, "a group name cannot be empty");
    }
    *name = (sxml_regex_read_name){.byte_offset = name_offset,
                                   .byte_count = name_byte_count};
    return SXML_STATUS_OK;
}

/* The number of the group named like the name just read, or 0. */
static uint32_t sxml_regex_compiler_find_name(
    const sxml_regex_compiler *compiler, sxml_regex_read_name name) {
    sxml_byte_view name_view = sxml_regex_compiler_name_view(compiler, name);
    size_t entry_offset = 0;
    while (entry_offset < compiler->name_table_byte_count) {
        uint32_t group_number =
            sxml_regex_compiler_read_name_table_field(compiler, entry_offset);
        uint32_t byte_count = sxml_regex_compiler_read_name_table_field(
            compiler, entry_offset + sizeof(uint32_t));
        if (byte_count == name_view.byte_count &&
            memcmp(compiler->name_table + entry_offset +
                       SXML_REGEX_NAME_ENTRY_HEADER_BYTE_COUNT,
                   name_view.bytes, byte_count) == 0) {
            return group_number;
        }
        entry_offset += SXML_REGEX_NAME_ENTRY_HEADER_BYTE_COUNT + byte_count;
    }
    return 0;
}

/*
 * Makes the name just read the name of group group_number: an entry of
 * the name table, and in the emitting pass the group's name in the
 * compiled regex. A name used twice is FORX0002 (F&O 4.0, section 6.1.5).
 */
static sxml_status sxml_regex_compiler_name_group(
    sxml_regex_compiler *compiler, sxml_regex_read_name name,
    uint32_t group_number) {
    if (sxml_regex_compiler_find_name(compiler, name) != 0) {
        return SXML_REGEX_INVALID(compiler,
                                  "two capturing groups have the same name");
    }
    if (name.byte_count > UINT32_MAX - compiler->counts.group_name_byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    uint32_t name_byte_count = (uint32_t)name.byte_count;
    uint8_t *entry = compiler->name_table + compiler->name_table_byte_count;
    memcpy(entry, &group_number, sizeof(group_number));
    memcpy(entry + sizeof(uint32_t), &name_byte_count, sizeof(name_byte_count));
    compiler->name_table_byte_count = name.byte_offset + name.byte_count;
    uint32_t name_offset = compiler->counts.group_name_byte_count;
    compiler->counts.group_name_byte_count = name_offset + name_byte_count;
    if (compiler->regex != NULL) {
        memcpy(compiler->regex->group_names + name_offset,
               compiler->name_table + name.byte_offset, name.byte_count);
        compiler->regex->groups[group_number].name_offset = name_offset;
        compiler->regex->groups[group_number].name_byte_count = name_byte_count;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Back-references */

/* True when group_number is a group whose ")" has been read. */
static bool sxml_regex_compiler_group_is_closed(
    const sxml_regex_compiler *compiler, uint32_t group_number) {
    if (group_number == 0 || group_number > compiler->counts.group_count) {
        return false;
    }
    for (size_t frame_index = 1; frame_index < compiler->frame_count;
         frame_index++) {
        if (compiler->frames[frame_index].group_number == group_number) {
            return false;
        }
    }
    return true;
}

/*
 * Reads the digits of a back-reference after its first digit (F&O 4.0,
 * section 6.1.6): a further digit belongs to it only when the longer
 * number is a group whose "(" comes before.
 */
static uint32_t sxml_regex_compiler_read_back_reference(
    sxml_regex_compiler *compiler, uint32_t first_digit) {
    uint32_t group_number = first_digit - '0';
    uint32_t codepoint = 0;
    while (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint >= '0' &&
           codepoint <= '9') {
        uint64_t longer_number = (uint64_t)group_number * 10U + (codepoint - '0');
        if (longer_number > compiler->counts.group_count) {
            break;
        }
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        group_number = (uint32_t)longer_number;
    }
    return group_number;
}

/* ---------------------------------------------------------------- Parsing */

/*
 * The parser reads the regex in one loop over pieces, with the open
 * groups on an explicit stack (docs/DECISIONS.md D71), and emits each
 * atom once:
 * - a group's "(" pushes a frame and its ")" pops it, after which the
 *   group is an atom like any other;
 * - a "|" inserts the split that prefers the branch before it at the
 *   branch's start, and ends the branch with a jump to the group's end
 *   (F&O 4.0, section 6.1.1: the first matching branch is chosen);
 * - a quantifier after an atom inserts what goes before the atom's first
 *   copy and appends the other copies.
 */

/* The innermost open frame. */
static sxml_regex_group_frame *sxml_regex_compiler_top_frame(
    sxml_regex_compiler *compiler) {
    return &compiler->frames[compiler->frame_count - 1];
}

static bool sxml_regex_frame_kind_is_lookbehind(sxml_regex_frame_kind kind) {
    return kind == SXML_REGEX_FRAME_KIND_POSITIVE_LOOKBEHIND ||
           kind == SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKBEHIND;
}

static bool sxml_regex_frame_kind_is_lookahead(sxml_regex_frame_kind kind) {
    return kind == SXML_REGEX_FRAME_KIND_POSITIVE_LOOKAHEAD ||
           kind == SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKAHEAD;
}

/* True inside a lookbehind assertion, whose regex holds only branches of
   single characters (F&O 4.0, section 6.1.3, simpleRegExp). */
static bool sxml_regex_compiler_is_in_lookbehind(
    sxml_regex_compiler *compiler) {
    return sxml_regex_frame_kind_is_lookbehind(
        sxml_regex_compiler_top_frame(compiler)->kind);
}

/* The longest name of a "(*" assertion. */
#define SXML_REGEX_MAXIMUM_ASSERTION_NAME_BYTE_COUNT 24U

/* The "(*name:" spellings of the assertions (section 6.1.3). */
typedef struct sxml_regex_assertion_name {
    const char *name;
    sxml_regex_frame_kind kind;
} sxml_regex_assertion_name;

static const sxml_regex_assertion_name sxml_regex_assertion_names[] = {
    {"positive_lookahead", SXML_REGEX_FRAME_KIND_POSITIVE_LOOKAHEAD},
    {"negative_lookahead", SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKAHEAD},
    {"positive_lookbehind", SXML_REGEX_FRAME_KIND_POSITIVE_LOOKBEHIND},
    {"negative_lookbehind", SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKBEHIND},
};

/* Reads "name:" after "(*" and writes the assertion it names to *kind. */
static sxml_status sxml_regex_compiler_parse_assertion_name(
    sxml_regex_compiler *compiler, sxml_regex_frame_kind *kind) {
    char name[SXML_REGEX_MAXIMUM_ASSERTION_NAME_BYTE_COUNT];
    size_t name_byte_count = 0;
    for (;;) {
        uint32_t codepoint = 0;
        if (!sxml_regex_compiler_next(compiler, &codepoint)) {
            return SXML_REGEX_INVALID(compiler, "unterminated \"(*\"");
        }
        if (codepoint == ':') {
            break;
        }
        if (!((codepoint >= 'a' && codepoint <= 'z') || codepoint == '_') ||
            name_byte_count == SXML_REGEX_MAXIMUM_ASSERTION_NAME_BYTE_COUNT) {
            return SXML_REGEX_INVALID(compiler, "unknown \"(*\" assertion");
        }
        name[name_byte_count] = (char)codepoint;
        name_byte_count += 1;
    }
    for (size_t name_index = 0;
         name_index < sizeof(sxml_regex_assertion_names) /
                          sizeof(sxml_regex_assertion_names[0]);
         name_index++) {
        const char *known_name = sxml_regex_assertion_names[name_index].name;
        if (strlen(known_name) == name_byte_count &&
            memcmp(known_name, name, name_byte_count) == 0) {
            *kind = sxml_regex_assertion_names[name_index].kind;
            return SXML_STATUS_OK;
        }
    }
    return SXML_REGEX_INVALID(compiler, "unknown \"(*\" assertion");
}

/* What the "(" of a group or assertion introduces. */
typedef struct sxml_regex_group_opening {
    sxml_regex_frame_kind kind;
    bool is_capturing;
    bool has_name;
    sxml_regex_read_name name;
} sxml_regex_group_opening;

/*
 * Reads what follows "(" (F&O 4.0, section 6.1.3, atom and assertion):
 * "?:", "?<name>", "?=", "?!", "?<=", "?<!", "(*name:" or nothing (a
 * capturing group).
 */
static sxml_status sxml_regex_compiler_parse_group_opening(
    sxml_regex_compiler *compiler, sxml_regex_group_opening *opening) {
    *opening = (sxml_regex_group_opening){
        .kind = SXML_REGEX_FRAME_KIND_GROUP,
        .is_capturing = true,
        .has_name = false,
        .name = {.byte_offset = 0, .byte_count = 0},
    };
    uint32_t codepoint = 0;
    if (compiler->flags.is_xsd_pattern &&
        sxml_regex_compiler_peek(compiler, &codepoint) &&
        (codepoint == '*' || codepoint == '?')) {
        /* XSD 1.1 Part 2, G.1: every group is a plain "(" regExp ")". */
        return SXML_REGEX_INVALID(
            compiler, "an XSD pattern has no \"(?\" or \"(*\" groups");
    }
    if (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint == '*') {
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        opening->is_capturing = false;
        return sxml_regex_compiler_parse_assertion_name(compiler,
                                                        &opening->kind);
    }
    if (!sxml_regex_compiler_peek(compiler, &codepoint) || codepoint != '?') {
        return SXML_STATUS_OK;
    }
    (void)sxml_regex_compiler_next(compiler, &codepoint);
    if (!sxml_regex_compiler_next(compiler, &codepoint)) {
        return SXML_REGEX_INVALID(compiler, "unterminated group");
    }
    opening->is_capturing = false;
    switch (codepoint) {
    case ':':
        return SXML_STATUS_OK;
    case '=':
        opening->kind = SXML_REGEX_FRAME_KIND_POSITIVE_LOOKAHEAD;
        return SXML_STATUS_OK;
    case '!':
        opening->kind = SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKAHEAD;
        return SXML_STATUS_OK;
    case '<':
        break;
    default:
        return SXML_REGEX_INVALID(compiler, "unknown \"(?\" construct");
    }
    uint32_t following_codepoint = 0;
    if (sxml_regex_compiler_peek(compiler, &following_codepoint) &&
        (following_codepoint == '=' || following_codepoint == '!')) {
        (void)sxml_regex_compiler_next(compiler, &following_codepoint);
        opening->kind = following_codepoint == '='
                            ? SXML_REGEX_FRAME_KIND_POSITIVE_LOOKBEHIND
                            : SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKBEHIND;
        return SXML_STATUS_OK;
    }
    opening->is_capturing = true;
    opening->has_name = true;
    return sxml_regex_compiler_read_group_name(compiler, &opening->name);
}

/*
 * Reads a group or assertion after its "(" up to its body, numbers and
 * names a capturing group and pushes its frame. An assertion starts with
 * its LOOK_START; each branch of a lookbehind assertion with a STEP_BACK
 * whose count its end writes. atom_start is where the group's atom
 * started.
 */
static sxml_status sxml_regex_compiler_open_group(
    sxml_regex_compiler *compiler, const sxml_regex_atom_start *atom_start) {
    if (sxml_regex_compiler_group_depth(compiler) >=
        compiler->maximum_group_depth) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (sxml_regex_compiler_is_in_lookbehind(compiler)) {
        /* Section 6.1.8.5. */
        return SXML_REGEX_INVALID(
            compiler, "a lookbehind assertion cannot contain parentheses");
    }
    bool is_in_lookahead =
        sxml_regex_compiler_top_frame(compiler)->is_in_lookahead;
    uint32_t parent_group_number = 0;
    for (size_t frame_index = compiler->frame_count; frame_index > 0; frame_index--) {
        if (compiler->frames[frame_index - 1].group_number != 0) {
            parent_group_number = compiler->frames[frame_index - 1].group_number;
            break;
        }
    }
    sxml_regex_group_opening opening = {.kind = SXML_REGEX_FRAME_KIND_GROUP};
    sxml_status status =
        sxml_regex_compiler_parse_group_opening(compiler, &opening);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t group_number = 0;
    if (opening.is_capturing) {
        group_number = compiler->counts.group_count + 1;
        compiler->counts.group_count = group_number;
        if (compiler->regex != NULL) {
            compiler->regex->groups[group_number] = (sxml_regex_group){
                .name_offset = 0,
                .name_byte_count = 0,
                .parent_group_number = parent_group_number,
                .is_in_lookahead = is_in_lookahead,
            };
        }
        if (opening.has_name) {
            status = sxml_regex_compiler_name_group(compiler, opening.name,
                                                    group_number);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_compiler_emit_operation(
                compiler, SXML_REGEX_OPERATION_SAVE, 2 * group_number);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    uint32_t look_start_index = 0;
    if (opening.kind != SXML_REGEX_FRAME_KIND_GROUP) {
        bool is_negative =
            opening.kind == SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKAHEAD ||
            opening.kind == SXML_REGEX_FRAME_KIND_NEGATIVE_LOOKBEHIND;
        status = sxml_regex_compiler_emit(
            compiler,
            (sxml_regex_instruction){
                .operation = is_negative
                                 ? SXML_REGEX_OPERATION_NEGATIVE_LOOK_START
                                 : SXML_REGEX_OPERATION_POSITIVE_LOOK_START,
                .operand = 0},
            &look_start_index);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    uint32_t branch_start = compiler->counts.instruction_count;
    if (sxml_regex_frame_kind_is_lookbehind(opening.kind)) {
        status = sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_STEP_BACK, 0);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return sxml_regex_compiler_push_frame(
        compiler,
        (sxml_regex_group_frame){
            .kind = opening.kind,
            .group_number = group_number,
            .branch_start = branch_start,
            .branch_ends = {.last_instruction_number = 0},
            .atom_start = *atom_start,
            .look_start_index = look_start_index,
            .branch_length = 0,
            .is_in_lookahead =
                is_in_lookahead || sxml_regex_frame_kind_is_lookahead(opening.kind),
        });
}

/* Ends the branches of the innermost frame at the current instruction;
   in a lookbehind assertion, the last branch's STEP_BACK gets its
   length. */
static void sxml_regex_compiler_end_branches(sxml_regex_compiler *compiler) {
    const sxml_regex_group_frame *frame = sxml_regex_compiler_top_frame(compiler);
    if (sxml_regex_frame_kind_is_lookbehind(frame->kind)) {
        sxml_regex_compiler_patch(compiler, frame->branch_start,
                                  frame->branch_length);
    }
    sxml_regex_pending_targets_resolve(compiler, frame->branch_ends,
                                       compiler->counts.instruction_count);
}

/*
 * Reads the ")" of the innermost group or assertion, ends it and pops its
 * frame into *closed_frame. An assertion ends with LOOK_END, after which
 * its LOOK_START goes on when its regex fails.
 */
static sxml_status sxml_regex_compiler_close_group(
    sxml_regex_compiler *compiler, sxml_regex_group_frame *closed_frame) {
    sxml_regex_compiler_end_branches(compiler);
    const sxml_regex_group_frame frame =
        compiler->frames[compiler->frame_count - 1];
    compiler->frame_count -= 1;
    uint32_t codepoint = 0;
    (void)sxml_regex_compiler_next(compiler, &codepoint);
    *closed_frame = frame;
    sxml_status status = SXML_STATUS_OK;
    if (frame.group_number != 0) {
        status = sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_SAVE, 2 * frame.group_number + 1);
    }
    if (status == SXML_STATUS_OK && frame.kind != SXML_REGEX_FRAME_KIND_GROUP) {
        status = sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_LOOK_END, 0);
        sxml_regex_compiler_patch(compiler, frame.look_start_index,
                                  compiler->counts.instruction_count);
    }
    return status;
}

/*
 * Reads a "|": the branch before it gets a split at its start that prefers
 * it, and a jump to the end of the group; the next branch starts after.
 * In a lookbehind assertion every branch starts with its own STEP_BACK.
 */
static sxml_status sxml_regex_compiler_start_next_branch(
    sxml_regex_compiler *compiler) {
    sxml_regex_group_frame *frame = sxml_regex_compiler_top_frame(compiler);
    bool is_lookbehind = sxml_regex_frame_kind_is_lookbehind(frame->kind);
    uint32_t codepoint = 0;
    (void)sxml_regex_compiler_next(compiler, &codepoint);
    uint32_t split_index = frame->branch_start;
    if (is_lookbehind) {
        sxml_regex_compiler_patch(compiler, split_index, frame->branch_length);
    }
    sxml_status status = sxml_regex_compiler_insert(
        compiler, (sxml_regex_instruction_range){
                      .first_instruction_index = split_index,
                      .instruction_count = 1,
                  });
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_compiler_write(
        compiler, split_index,
        (sxml_regex_instruction){
            .operation = SXML_REGEX_OPERATION_SPLIT_NEXT_FIRST, .operand = 0});
    uint32_t jump_index = 0;
    status = sxml_regex_compiler_emit(
        compiler,
        (sxml_regex_instruction){.operation = SXML_REGEX_OPERATION_JUMP,
                                 .operand = 0},
        &jump_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_pending_targets_add(compiler, &frame->branch_ends, jump_index);
    frame->branch_start = compiler->counts.instruction_count;
    sxml_regex_compiler_patch(compiler, split_index, frame->branch_start);
    if (is_lookbehind) {
        frame->branch_length = 0;
        status = sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_STEP_BACK, 0);
    }
    return status;
}

/* Fails when a quantifier follows: assertions take none (F&O 4.0,
   section 6.1.8), nor do the characters of a lookbehind assertion. */
static sxml_status sxml_regex_compiler_reject_quantifier(
    sxml_regex_compiler *compiler) {
    uint32_t codepoint = 0;
    if (sxml_regex_compiler_peek(compiler, &codepoint) &&
        (codepoint == '?' || codepoint == '*' || codepoint == '+' ||
         codepoint == '{')) {
        return sxml_regex_compiler_is_in_lookbehind(compiler)
                   ? SXML_REGEX_INVALID(compiler,
                                        "a lookbehind assertion cannot contain "
                                        "a quantifier")
                   : SXML_REGEX_INVALID(compiler,
                                        "an assertion cannot have a quantifier");
    }
    return SXML_STATUS_OK;
}

/* What an escape outside a class was. */
typedef struct sxml_regex_escape_outcome {
    /* \b or \B: an assertion, which takes no quantifier. */
    bool is_assertion;
} sxml_regex_escape_outcome;

/* Emits a back-reference to group_number, which must be closed already
   (F&O 4.0, section 6.1.6). */
static sxml_status sxml_regex_compiler_emit_back_reference(
    sxml_regex_compiler *compiler, uint32_t group_number) {
    if (!sxml_regex_compiler_group_is_closed(compiler, group_number)) {
        return SXML_REGEX_INVALID(
            compiler, "a back-reference must follow the \")\" of its group");
    }
    return sxml_regex_compiler_emit_operation(
        compiler, SXML_REGEX_OPERATION_BACK_REFERENCE, group_number);
}

/* Reads an escape outside a class after its backslash. */
static sxml_status sxml_regex_compiler_parse_escape(
    sxml_regex_compiler *compiler, sxml_regex_escape_outcome *outcome) {
    outcome->is_assertion = false;
    uint32_t escaped_codepoint = 0;
    if (!sxml_regex_compiler_next_escaped(compiler, &escaped_codepoint)) {
        return SXML_REGEX_INVALID(compiler, "\"\\\" at the end");
    }
    uint32_t codepoint = 0;
    if (sxml_regex_single_character_escape(escaped_codepoint, &codepoint)) {
        return sxml_regex_compiler_emit_literal(compiler, codepoint);
    }
    sxml_regex_class_escape escape = {.is_class_escape = false};
    sxml_status status = sxml_regex_compiler_parse_class_escape(
        compiler, escaped_codepoint, &escape);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (escape.is_class_escape) {
        uint32_t class_index = 0;
        status = sxml_regex_compiler_reserve_class(compiler, &class_index);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint32_t first_item_index = compiler->counts.class_item_count;
        status = sxml_regex_compiler_emit_class_item(compiler, escape.item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_regex_compiler_write_class(
            compiler, class_index,
            (sxml_regex_class){
                .first_item_index = first_item_index,
                .item_count = 1,
                .is_negated = false,
                .subtracted_class_number = 0,
            });
        return sxml_regex_compiler_emit_class_instruction(compiler,
                                                          class_index);
    }
    if (compiler->flags.is_xsd_pattern) {
        /* XSD 1.1 Part 2, G.4: no \b, \B or back-references. */
        return SXML_REGEX_INVALID(compiler, "unknown escape in an XSD pattern");
    }
    bool is_in_lookbehind = sxml_regex_compiler_is_in_lookbehind(compiler);
    if (escaped_codepoint == 'b' || escaped_codepoint == 'B') {
        if (is_in_lookbehind) {
            return SXML_REGEX_INVALID(
                compiler, "a lookbehind assertion cannot contain \\b or \\B");
        }
        /* Section 6.1.8.2. */
        outcome->is_assertion = true;
        return sxml_regex_compiler_emit_operation(
            compiler,
            escaped_codepoint == 'b' ? SXML_REGEX_OPERATION_WORD_BOUNDARY
                                     : SXML_REGEX_OPERATION_NOT_WORD_BOUNDARY,
            0);
    }
    bool is_back_reference = (escaped_codepoint >= '1' && escaped_codepoint <= '9') ||
                             escaped_codepoint == 'k';
    if (is_back_reference && is_in_lookbehind) {
        return SXML_REGEX_INVALID(
            compiler, "a lookbehind assertion cannot contain a back-reference");
    }
    if (escaped_codepoint >= '1' && escaped_codepoint <= '9') {
        return sxml_regex_compiler_emit_back_reference(
            compiler,
            sxml_regex_compiler_read_back_reference(compiler, escaped_codepoint));
    }
    if (escaped_codepoint == 'k') {
        /* \k<name> (section 6.1.6). */
        if (!sxml_regex_compiler_next(compiler, &codepoint) || codepoint != '<') {
            return SXML_REGEX_INVALID(compiler, "\"\\k\" must be followed by \"<\"");
        }
        sxml_regex_read_name name = {.byte_offset = 0};
        status = sxml_regex_compiler_read_group_name(compiler, &name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint32_t group_number = sxml_regex_compiler_find_name(compiler, name);
        if (group_number == 0) {
            return SXML_REGEX_INVALID(
                compiler, "\"\\k<name>\" names no capturing group");
        }
        return sxml_regex_compiler_emit_back_reference(compiler, group_number);
    }
    return SXML_REGEX_INVALID(compiler, "unknown escape");
}

typedef struct sxml_regex_quantifier {
    /* False when the atom has no quantifier. */
    bool is_present;
    uint32_t minimum_count;
    uint32_t maximum_count;
    bool is_unbounded;
    bool is_reluctant;
} sxml_regex_quantifier;

/* Reads the digits of a quantity; values above UINT32_MAX saturate (a
   counted loop then needs more than 2^32 characters to tell them apart,
   D147). */
static bool sxml_regex_compiler_read_quantity(sxml_regex_compiler *compiler,
                                              uint32_t *quantity) {
    uint64_t value = 0;
    bool has_digit = false;
    uint32_t codepoint = 0;
    while (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint >= '0' &&
           codepoint <= '9') {
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        has_digit = true;
        value = value * 10U + (codepoint - '0');
        if (value > UINT32_MAX) {
            value = UINT32_MAX;
        }
    }
    *quantity = (uint32_t)value;
    return has_digit;
}

/* Reads "{" quantity "}" after its "{" (quantRange, quantMin, QuantExact). */
static sxml_status sxml_regex_compiler_parse_quantity(
    sxml_regex_compiler *compiler, sxml_regex_quantifier *quantifier) {
    uint32_t codepoint = 0;
    if (!sxml_regex_compiler_read_quantity(compiler,
                                           &quantifier->minimum_count)) {
        return SXML_REGEX_INVALID(compiler, "a quantity needs a number");
    }
    quantifier->maximum_count = quantifier->minimum_count;
    if (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint == ',') {
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        if (!sxml_regex_compiler_read_quantity(compiler,
                                               &quantifier->maximum_count)) {
            quantifier->is_unbounded = true;
        } else if (quantifier->maximum_count < quantifier->minimum_count) {
            return SXML_REGEX_INVALID(compiler,
                                      "a quantity's maximum is below its "
                                      "minimum");
        }
    }
    if (!sxml_regex_compiler_next(compiler, &codepoint) || codepoint != '}') {
        return SXML_REGEX_INVALID(compiler, "a quantity must end with \"}\"");
    }
    return SXML_STATUS_OK;
}

/* Reads an optional quantifier (F&O 4.0, section 6.1.3, quantifier,
   with the reluctant "?" of section 6.1.4). */
static sxml_status sxml_regex_compiler_parse_quantifier(
    sxml_regex_compiler *compiler, sxml_regex_quantifier *quantifier) {
    uint32_t codepoint = 0;
    *quantifier = (sxml_regex_quantifier){
        .is_present = false,
        .minimum_count = 0,
        .maximum_count = 0,
        .is_unbounded = false,
        .is_reluctant = false,
    };
    if (!sxml_regex_compiler_peek(compiler, &codepoint)) {
        return SXML_STATUS_OK;
    }
    switch (codepoint) {
    case '?':
        quantifier->maximum_count = 1;
        break;
    case '*':
        quantifier->is_unbounded = true;
        break;
    case '+':
        quantifier->minimum_count = 1;
        quantifier->is_unbounded = true;
        break;
    case '{':
        break;
    default:
        return SXML_STATUS_OK;
    }
    (void)sxml_regex_compiler_next(compiler, &codepoint);
    if (codepoint == '{') {
        sxml_status status =
            sxml_regex_compiler_parse_quantity(compiler, quantifier);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint == '?') {
        if (compiler->flags.is_xsd_pattern) {
            /* XSD 1.1 Part 2, G.1: a piece has at most one quantifier. */
            return SXML_REGEX_INVALID(
                compiler, "an XSD pattern has no reluctant quantifiers");
        }
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        quantifier->is_reluctant = true;
    }
    quantifier->is_present = true;
    return SXML_STATUS_OK;
}

/* The split of a quantifier: a greedy one prefers another copy, a
   reluctant one the rest of the regex (section 6.1.4). */
static sxml_regex_operation sxml_regex_quantifier_split_operation(
    sxml_regex_quantifier quantifier) {
    return quantifier.is_reluctant ? SXML_REGEX_OPERATION_SPLIT_TARGET_FIRST
                                   : SXML_REGEX_OPERATION_SPLIT_NEXT_FIRST;
}

/*
 * Ends the unbounded loop whose split is at loop_start and whose progress
 * register is in progress_slot, after the copy of its atom: a check that
 * the iteration consumed something, a jump back, and the split's exit.
 */
static sxml_status sxml_regex_compiler_end_loop(sxml_regex_compiler *compiler,
                                                uint32_t loop_start,
                                                uint32_t progress_slot) {
    sxml_status status = sxml_regex_compiler_emit_operation(
        compiler, SXML_REGEX_OPERATION_CHECK_PROGRESS, progress_slot);
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_JUMP, loop_start);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_compiler_patch(compiler, loop_start,
                              compiler->counts.instruction_count);
    return SXML_STATUS_OK;
}

/* For x* and x*?: puts the loop's split and progress save before the atom
   just emitted, and ends the loop after it. */
static sxml_status sxml_regex_compiler_wrap_in_loop(
    sxml_regex_compiler *compiler, const sxml_regex_atom_copy_source *atom,
    sxml_regex_quantifier quantifier) {
    uint32_t loop_start = atom->instructions.first_instruction_index;
    sxml_status status = sxml_regex_compiler_insert(
        compiler, (sxml_regex_instruction_range){
                      .first_instruction_index = loop_start,
                      .instruction_count = 2,
                  });
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t progress_slot = sxml_regex_compiler_add_register(compiler);
    sxml_regex_compiler_write(
        compiler, loop_start,
        (sxml_regex_instruction){
            .operation = sxml_regex_quantifier_split_operation(quantifier),
            .operand = 0});
    sxml_regex_compiler_write(
        compiler, loop_start + 1,
        (sxml_regex_instruction){.operation = SXML_REGEX_OPERATION_SAVE,
                                 .operand = progress_slot});
    return sxml_regex_compiler_end_loop(compiler, loop_start, progress_slot);
}

/* For x{n,} with n at least 1: appends a loop over another copy. */
static sxml_status sxml_regex_compiler_emit_loop(
    sxml_regex_compiler *compiler, const sxml_regex_atom_copy_source *atom,
    sxml_regex_quantifier quantifier) {
    uint32_t loop_start = 0;
    sxml_status status = sxml_regex_compiler_emit(
        compiler,
        (sxml_regex_instruction){
            .operation = sxml_regex_quantifier_split_operation(quantifier),
            .operand = 0},
        &loop_start);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t progress_slot = sxml_regex_compiler_add_register(compiler);
    status = sxml_regex_compiler_emit_operation(
        compiler, SXML_REGEX_OPERATION_SAVE, progress_slot);
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_compiler_emit_copy(compiler, atom);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_regex_compiler_end_loop(compiler, loop_start, progress_slot);
}

/*
 * The most instructions the copies of one quantified atom may add; a
 * counted repetition that needs more becomes a counted loop (D147). Copies
 * run faster than a loop, which pays four instructions and three backtrack
 * entries per iteration, but their number grows with the count.
 */
#define SXML_REGEX_LARGEST_EXPANSION 256U

/*
 * True when quantifier on an atom of instruction_count instructions is
 * compiled as a counted loop: the instructions its copies would add (each
 * at least one, so that an atom that emits nothing still counts) pass
 * SXML_REGEX_LARGEST_EXPANSION. x* and x*? never are: they need no copy.
 */
static bool sxml_regex_quantifier_needs_counted_loop(
    sxml_regex_quantifier quantifier, uint32_t instruction_count) {
    uint64_t copy_size = instruction_count == 0 ? 1U : instruction_count;
    uint64_t added_count = 0;
    if (quantifier.is_unbounded) {
        /* minimum copies, the first of them the atom, plus the loop's
           copy. */
        added_count = (uint64_t)quantifier.minimum_count * copy_size;
    } else {
        /* maximum copies, plus a split before each optional one. */
        added_count =
            ((uint64_t)quantifier.maximum_count - 1U) * copy_size +
            (quantifier.maximum_count - quantifier.minimum_count);
    }
    return added_count > SXML_REGEX_LARGEST_EXPANSION;
}

/* Takes the next entry of counted_loops. */
static sxml_status sxml_regex_compiler_add_counted_loop(
    sxml_regex_compiler *compiler, uint32_t *loop_index) {
    if (compiler->counts.counted_loop_count >=
        compiler->maximum_instruction_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    *loop_index = compiler->counts.counted_loop_count;
    compiler->counts.counted_loop_count += 1;
    return SXML_STATUS_OK;
}

/*
 * Turns the atom just emitted into a counted loop (D147; the code is
 * described at SXML_REGEX_OPERATION_COUNTED_LOOP_START): four
 * instructions inserted before it, COUNTED_LOOP_END after it.
 */
static sxml_status sxml_regex_compiler_emit_counted_loop(
    sxml_regex_compiler *compiler, const sxml_regex_atom_copy_source *atom,
    sxml_regex_quantifier quantifier) {
    uint32_t loop_start = atom->instructions.first_instruction_index;
    uint32_t loop_index = 0;
    sxml_status status = sxml_regex_compiler_insert(
        compiler, (sxml_regex_instruction_range){
                      .first_instruction_index = loop_start,
                      .instruction_count = 4,
                  });
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_compiler_add_counted_loop(compiler, &loop_index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t counter_slot = sxml_regex_compiler_add_register(compiler);
    uint32_t progress_slot = sxml_regex_compiler_add_register(compiler);
    if (compiler->regex != NULL) {
        compiler->regex->counted_loops[loop_index] = (sxml_regex_counted_loop){
            .minimum_count = quantifier.minimum_count,
            .maximum_count = quantifier.maximum_count,
            .is_unbounded = quantifier.is_unbounded,
            .is_reluctant = quantifier.is_reluctant,
            .counter_slot = counter_slot,
            .progress_slot = progress_slot,
        };
    }
    static const sxml_regex_operation head_operations[] = {
        SXML_REGEX_OPERATION_COUNTED_LOOP_START,
        SXML_REGEX_OPERATION_COUNTED_LOOP_CHOOSE,
        SXML_REGEX_OPERATION_JUMP,
        SXML_REGEX_OPERATION_COUNTED_LOOP_BODY,
    };
    for (uint32_t head_index = 0; head_index < 4; head_index++) {
        sxml_regex_compiler_write(
            compiler, loop_start + head_index,
            (sxml_regex_instruction){
                .operation = head_operations[head_index],
                .operand = head_operations[head_index] ==
                                   SXML_REGEX_OPERATION_JUMP
                               ? 0
                               : loop_index});
    }
    status = sxml_regex_compiler_emit_operation(
        compiler, SXML_REGEX_OPERATION_COUNTED_LOOP_END, loop_start + 1);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_compiler_patch(compiler, loop_start + 2,
                              compiler->counts.instruction_count);
    return SXML_STATUS_OK;
}

/*
 * Applies a quantifier to the atom just emitted, which started at
 * atom_start: the minimum number of copies, then a loop or the optional
 * copies, each optional copy after a split to the end (section 6.1.4 for
 * reluctance). The atom's own instructions are the first copy; when the
 * minimum is 0, what precedes the first optional copy is inserted before
 * them.
 */
static sxml_status sxml_regex_compiler_apply_quantifier(
    sxml_regex_compiler *compiler, const sxml_regex_atom_start *atom_start,
    sxml_regex_quantifier quantifier) {
    if (!quantifier.is_present) {
        return SXML_STATUS_OK;
    }
    if (quantifier.maximum_count == 0 && !quantifier.is_unbounded) {
        /* x{0}: parsed for its errors and group numbers, then dropped;
           what it wrote still had to fit the arrays. */
        sxml_regex_compiler_note_array_counts(compiler);
        uint32_t group_count = compiler->counts.group_count;
        uint32_t group_name_byte_count = compiler->counts.group_name_byte_count;
        compiler->counts = atom_start->counts;
        compiler->counts.group_count = group_count;
        compiler->counts.group_name_byte_count = group_name_byte_count;
        return SXML_STATUS_OK;
    }
    sxml_regex_atom_copy_source atom = {
        .instructions =
            {
                .first_instruction_index =
                    atom_start->counts.instruction_count,
                .instruction_count = compiler->counts.instruction_count -
                                     atom_start->counts.instruction_count,
            },
        .first_register_number = atom_start->counts.register_count,
        .register_count = compiler->counts.register_count -
                          atom_start->counts.register_count,
        .atom_parse_count =
            compiler->atom_parse_count - atom_start->atom_parse_count,
    };
    if (sxml_regex_quantifier_needs_counted_loop(
            quantifier, atom.instructions.instruction_count)) {
        return sxml_regex_compiler_emit_counted_loop(compiler, &atom,
                                                     quantifier);
    }
    sxml_regex_pending_targets exits = {.last_instruction_number = 0};
    sxml_status status = SXML_STATUS_OK;
    if (quantifier.minimum_count == 0) {
        if (quantifier.is_unbounded) {
            return sxml_regex_compiler_wrap_in_loop(compiler, &atom,
                                                    quantifier);
        }
        uint32_t split_index = atom.instructions.first_instruction_index;
        status = sxml_regex_compiler_insert(
            compiler, (sxml_regex_instruction_range){
                          .first_instruction_index = split_index,
                          .instruction_count = 1,
                      });
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_regex_compiler_write(
            compiler, split_index,
            (sxml_regex_instruction){
                .operation = sxml_regex_quantifier_split_operation(quantifier),
                .operand = 0});
        sxml_regex_pending_targets_add(compiler, &exits, split_index);
        atom.instructions.first_instruction_index = split_index + 1;
    }
    /* Copies present so far: the atom's own instructions. */
    uint32_t copy_count = 1;
    for (; copy_count < quantifier.minimum_count; copy_count++) {
        status = sxml_regex_compiler_emit_copy(compiler, &atom);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (quantifier.is_unbounded) {
        return sxml_regex_compiler_emit_loop(compiler, &atom, quantifier);
    }
    for (; copy_count < quantifier.maximum_count; copy_count++) {
        uint32_t split_index = 0;
        status = sxml_regex_compiler_emit(
            compiler,
            (sxml_regex_instruction){
                .operation = sxml_regex_quantifier_split_operation(quantifier),
                .operand = 0},
            &split_index);
        if (status == SXML_STATUS_OK) {
            sxml_regex_pending_targets_add(compiler, &exits, split_index);
            status = sxml_regex_compiler_emit_copy(compiler, &atom);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_regex_pending_targets_resolve(compiler, exits,
                                       compiler->counts.instruction_count);
    return SXML_STATUS_OK;
}

/* Reads the optional quantifier of the atom just emitted and applies it. */
static sxml_status sxml_regex_compiler_end_atom(
    sxml_regex_compiler *compiler, const sxml_regex_atom_start *atom_start) {
    sxml_regex_quantifier quantifier = {.is_present = false};
    sxml_status status =
        sxml_regex_compiler_parse_quantifier(compiler, &quantifier);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_regex_compiler_apply_quantifier(compiler, atom_start,
                                                quantifier);
}

/*
 * Reads one atom (F&O 4.0, section 6.1.3, atom) and emits it with its
 * quantifier; a group's "(" only opens the group, which its ")" ends. In
 * a lookbehind assertion an atom is one character without a quantifier
 * (simplePiece), counted for the branch's STEP_BACK.
 */
static sxml_status sxml_regex_compiler_parse_atom(
    sxml_regex_compiler *compiler) {
    sxml_regex_atom_start atom_start = {
        .counts = compiler->counts,
        .atom_parse_count = compiler->atom_parse_count,
    };
    if (compiler->atom_parse_count >= compiler->maximum_instruction_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    compiler->atom_parse_count += 1;
    uint32_t codepoint = 0;
    if (!sxml_regex_compiler_next(compiler, &codepoint)) {
        return SXML_REGEX_INVALID(compiler, "missing atom");
    }
    sxml_status status = SXML_STATUS_OK;
    sxml_regex_escape_outcome outcome = {.is_assertion = false};
    switch (codepoint) {
    case '(':
        return sxml_regex_compiler_open_group(compiler, &atom_start);
    case '[': {
        uint32_t class_index = 0;
        status =
            sxml_regex_compiler_parse_class_expression(compiler, &class_index);
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_compiler_emit_class_instruction(compiler,
                                                                class_index);
        }
        break;
    }
    case '.':
        status = sxml_regex_compiler_emit_operation(
            compiler,
            compiler->flags.is_dot_all ? SXML_REGEX_OPERATION_ANY
                                       : SXML_REGEX_OPERATION_ANY_BUT_NEWLINE,
            0);
        break;
    case '\\':
        status = sxml_regex_compiler_parse_escape(compiler, &outcome);
        break;
    case '?':
    case '*':
    case '+':
    case '{':
        return SXML_REGEX_INVALID(compiler,
                                  "a quantifier must follow an atom");
    case '}':
    case ']':
    case ')':
    case '|':
        return SXML_REGEX_INVALID(compiler,
                                  "a metacharacter must be escaped here");
    default:
        status = sxml_regex_compiler_emit_literal(compiler, codepoint);
        break;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (outcome.is_assertion) {
        return sxml_regex_compiler_reject_quantifier(compiler);
    }
    if (sxml_regex_compiler_is_in_lookbehind(compiler)) {
        sxml_regex_compiler_top_frame(compiler)->branch_length += 1;
        return sxml_regex_compiler_reject_quantifier(compiler);
    }
    return sxml_regex_compiler_end_atom(compiler, &atom_start);
}

/* Reads "^" or "$" (already peeked), which takes no quantifier. */
static sxml_status sxml_regex_compiler_parse_assertion(
    sxml_regex_compiler *compiler) {
    if (sxml_regex_compiler_is_in_lookbehind(compiler)) {
        /* Section 6.1.8.5: only single characters. */
        return SXML_REGEX_INVALID(
            compiler, "a lookbehind assertion cannot contain \"^\" or \"$\"");
    }
    uint32_t codepoint = 0;
    (void)sxml_regex_compiler_next(compiler, &codepoint);
    sxml_regex_operation operation =
        codepoint == '^'
            ? (compiler->flags.is_multi_line ? SXML_REGEX_OPERATION_LINE_START
                                             : SXML_REGEX_OPERATION_STRING_START)
            : (compiler->flags.is_multi_line ? SXML_REGEX_OPERATION_LINE_END
                                             : SXML_REGEX_OPERATION_STRING_END);
    /* F&O 4.0, section 6.1.8. */
    sxml_status status = sxml_regex_compiler_reject_quantifier(compiler);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_regex_compiler_emit_operation(compiler, operation, 0);
}

/* Reads one step of a regExp inside a group: a "|", the group's ")", an
   assertion or an atom. */
static sxml_status sxml_regex_compiler_parse_step(
    sxml_regex_compiler *compiler) {
    uint32_t codepoint = 0;
    bool has_codepoint = sxml_regex_compiler_peek(compiler, &codepoint);
    if (!has_codepoint || codepoint == ')') {
        if (!has_codepoint) {
            return SXML_REGEX_INVALID(compiler, "\"(\" without \")\"");
        }
        sxml_regex_group_frame closed_frame = {.kind = SXML_REGEX_FRAME_KIND_GROUP};
        sxml_status status =
            sxml_regex_compiler_close_group(compiler, &closed_frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (closed_frame.kind != SXML_REGEX_FRAME_KIND_GROUP) {
            return sxml_regex_compiler_reject_quantifier(compiler);
        }
        return sxml_regex_compiler_end_atom(compiler, &closed_frame.atom_start);
    }
    if (codepoint == '|') {
        return sxml_regex_compiler_start_next_branch(compiler);
    }
    if ((codepoint == '^' || codepoint == '$') &&
        !compiler->flags.is_xsd_pattern) {
        /* XSD 1.1 Part 2, G.1: in a pattern they are normal characters. */
        return sxml_regex_compiler_parse_assertion(compiler);
    }
    return sxml_regex_compiler_parse_atom(compiler);
}

/* Reads the whole regex (F&O 4.0, section 6.1.3, regExp) with the frame
   of the whole regex at the bottom of the group stack. */
static sxml_status sxml_regex_compiler_parse_regex(
    sxml_regex_compiler *compiler) {
    sxml_status status = sxml_regex_compiler_push_frame(
        compiler, (sxml_regex_group_frame){
                      .kind = SXML_REGEX_FRAME_KIND_GROUP,
                      .group_number = 0,
                      .branch_start = compiler->counts.instruction_count,
                      .branch_ends = {.last_instruction_number = 0},
                      .atom_start = {.atom_parse_count = 0},
                      .look_start_index = 0,
                      .branch_length = 0,
                      .is_in_lookahead = false,
                  });
    while (status == SXML_STATUS_OK) {
        uint32_t codepoint = 0;
        bool has_codepoint = sxml_regex_compiler_peek(compiler, &codepoint);
        if (compiler->frame_count == 1 && (!has_codepoint || codepoint == ')')) {
            sxml_regex_compiler_end_branches(compiler);
            return has_codepoint
                       ? SXML_REGEX_INVALID(compiler, "\")\" without \"(\"")
                       : SXML_STATUS_OK;
        }
        status = sxml_regex_compiler_parse_step(compiler);
    }
    return status;
}

/* With the q flag every character stands for itself (section 6.2). */
static sxml_status sxml_regex_compiler_parse_literal_text(
    sxml_regex_compiler *compiler) {
    uint32_t codepoint = 0;
    while (sxml_regex_compiler_next_raw(compiler, &codepoint)) {
        sxml_status status = sxml_regex_compiler_emit_literal(compiler, codepoint);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* The group frames sxml_regex_compile holds itself. */
#define SXML_REGEX_INLINE_FRAME_COUNT (SXML_REGEX_INLINE_GROUP_DEPTH + 1U)

/*
 * Runs one pass of the compiler: counting when regex is NULL, otherwise
 * writing into regex, whose arrays are sized by a counting pass and whose
 * group_count that pass found.
 */
static sxml_status sxml_regex_compile(
    const sxml_regex_configuration *configuration, sxml_regex *regex,
    sxml_regex_counts *counts) {
    sxml_regex_group_frame inline_frames[SXML_REGEX_INLINE_FRAME_COUNT];
    uint8_t inline_name_table[SXML_REGEX_INLINE_NAME_TABLE_BYTE_COUNT];
    sxml_regex_compiler compiler = {
        .regex_text = configuration->regex_text,
        .byte_offset = 0,
        .is_in_class = false,
        .flags = configuration->flags,
        .regex = regex,
        .counts = {.instruction_count = 0},
        .array_counts = {.instruction_count = 0},
        .total_group_count = regex == NULL ? 0 : regex->group_count,
        .maximum_instruction_count =
            configuration->maximum_instruction_count == 0
                ? SXML_REGEX_DEFAULT_MAXIMUM_INSTRUCTION_COUNT
                : configuration->maximum_instruction_count,
        .maximum_group_depth = configuration->maximum_group_depth == 0
                                   ? SXML_REGEX_DEFAULT_MAXIMUM_GROUP_DEPTH
                                   : configuration->maximum_group_depth,
        .atom_parse_count = 0,
        .frames = inline_frames,
        .frame_count = 0,
        .frame_capacity = SXML_REGEX_INLINE_FRAME_COUNT,
        .inline_frames = inline_frames,
        .name_table = inline_name_table,
        .name_table_byte_count = 0,
        .name_table_capacity = SXML_REGEX_INLINE_NAME_TABLE_BYTE_COUNT,
        .inline_name_table = inline_name_table,
        .stack_allocator = configuration->stack_allocator,
        .error_record = configuration->error_record,
    };
    if (compiler.flags.is_xsd_pattern) {
        compiler.flags = (sxml_regex_flags){.is_xsd_pattern = true};
    }
    sxml_status status = SXML_STATUS_OK;
    if (compiler.flags.is_literal) {
        status = sxml_regex_compiler_parse_literal_text(&compiler);
    } else {
        status = sxml_regex_compiler_parse_regex(&compiler);
    }
    sxml_regex_compiler_release_frames(&compiler);
    sxml_regex_compiler_release_names(&compiler);
    if (status == SXML_STATUS_OK && compiler.flags.is_xsd_pattern) {
        /* XSD 1.1 Part 2, 4.3.4.4: the pattern matches the whole value. */
        status = sxml_regex_compiler_emit_operation(
            &compiler, SXML_REGEX_OPERATION_STRING_END, 0);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_compiler_emit_operation(
            &compiler, SXML_REGEX_OPERATION_MATCH, 0);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_compiler_note_array_counts(&compiler);
    *counts = compiler.counts;
    counts->instruction_count = compiler.array_counts.instruction_count;
    counts->class_count = compiler.array_counts.class_count;
    counts->class_item_count = compiler.array_counts.class_item_count;
    counts->counted_loop_count = compiler.array_counts.counted_loop_count;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Regex lifecycle */

/* Where the parts of a compiled regex go in its one block. */
typedef struct sxml_regex_layout {
    sxml_memory_requirements requirements;
    size_t instructions_offset;
    size_t classes_offset;
    size_t class_items_offset;
    size_t counted_loops_offset;
    size_t groups_offset;
    size_t group_names_offset;
} sxml_regex_layout;

/* Lays out the header and the six arrays for counts. */
static sxml_status sxml_regex_compute_layout(sxml_regex_counts counts,
                                             sxml_regex_layout *layout) {
    sxml_regex_layout new_layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
    };
    size_t header_offset = 0;
    sxml_status status = sxml_memory_requirements_append(
        &new_layout.requirements,
        (sxml_memory_requirements){.byte_count = sizeof(struct sxml_regex),
                                   .alignment = alignof(struct sxml_regex)},
        &header_offset);
    size_t array_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(counts.instruction_count,
                                    sizeof(sxml_regex_instruction),
                                    &array_byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){
                .byte_count = array_byte_count,
                .alignment = alignof(sxml_regex_instruction)},
            &new_layout.instructions_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(counts.class_count,
                                    sizeof(sxml_regex_class),
                                    &array_byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){.byte_count = array_byte_count,
                                       .alignment = alignof(sxml_regex_class)},
            &new_layout.classes_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(counts.class_item_count,
                                    sizeof(sxml_regex_class_item),
                                    &array_byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){
                .byte_count = array_byte_count,
                .alignment = alignof(sxml_regex_class_item)},
            &new_layout.class_items_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(counts.counted_loop_count,
                                    sizeof(sxml_regex_counted_loop),
                                    &array_byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){
                .byte_count = array_byte_count,
                .alignment = alignof(sxml_regex_counted_loop)},
            &new_layout.counted_loops_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply((size_t)counts.group_count + 1,
                                    sizeof(sxml_regex_group),
                                    &array_byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){.byte_count = array_byte_count,
                                       .alignment = alignof(sxml_regex_group)},
            &new_layout.groups_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){
                .byte_count = counts.group_name_byte_count, .alignment = 1},
            &new_layout.group_names_offset);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = new_layout;
    return SXML_STATUS_OK;
}

/* Runs the counting pass and lays out its result. */
static sxml_status sxml_regex_measure(
    const sxml_regex_configuration *configuration, sxml_regex_counts *counts,
    sxml_regex_layout *layout) {
    if (configuration->maximum_instruction_count >
        SXML_REGEX_MAXIMUM_INSTRUCTION_COUNT_LIMIT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_regex_counts measured_counts = {.instruction_count = 0};
    sxml_status status =
        sxml_regex_compile(configuration, NULL, &measured_counts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_regex_compute_layout(measured_counts, layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *counts = measured_counts;
    return SXML_STATUS_OK;
}

sxml_status sxml_regex_memory_requirements(
    const sxml_regex_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_regex_counts counts = {.instruction_count = 0};
    sxml_regex_layout layout = {.requirements = {.byte_count = 0,
                                                 .alignment = 1}};
    sxml_status status = sxml_regex_measure(configuration, &counts, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

/* The address of an array at offset within memory; NULL when empty. */
static void *sxml_regex_array_at(sxml_memory_view memory, size_t offset,
                                 size_t element_count) {
    return element_count == 0 ? NULL : memory.bytes + offset;
}

sxml_status sxml_regex_initialize(sxml_memory_view memory,
                                  const sxml_regex_configuration *configuration,
                                  sxml_regex **regex) {
    sxml_regex_counts counts = {.instruction_count = 0};
    sxml_regex_layout layout = {.requirements = {.byte_count = 0,
                                                 .alignment = 1}};
    sxml_status status = sxml_regex_measure(configuration, &counts, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, layout.requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_regex *new_regex = placement;
    *new_regex = (sxml_regex){
        .flags = configuration->flags.is_xsd_pattern
                     ? (sxml_regex_flags){.is_xsd_pattern = true}
                     : configuration->flags,
        .group_count = counts.group_count,
        /* Below 2^31: every group and register has its own instruction,
           and instructions are at most 2^28 (the configuration limit). */
        .slot_count = 2 * (counts.group_count + 1) + counts.register_count,
        .instruction_count = counts.instruction_count,
        .class_count = counts.class_count,
        .class_item_count = counts.class_item_count,
        .instructions = sxml_regex_array_at(memory, layout.instructions_offset,
                                            counts.instruction_count),
        .classes = sxml_regex_array_at(memory, layout.classes_offset,
                                       counts.class_count),
        .class_items = sxml_regex_array_at(memory, layout.class_items_offset,
                                           counts.class_item_count),
        .counted_loop_count = counts.counted_loop_count,
        .counted_loops = sxml_regex_array_at(
            memory, layout.counted_loops_offset, counts.counted_loop_count),
        .group_name_byte_count = counts.group_name_byte_count,
        .groups = sxml_regex_array_at(memory, layout.groups_offset,
                                      (size_t)counts.group_count + 1),
        .group_names = sxml_regex_array_at(memory, layout.group_names_offset,
                                           counts.group_name_byte_count),
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    new_regex->groups[0] = (sxml_regex_group){.name_offset = 0,
                                              .name_byte_count = 0,
                                              .parent_group_number = 0,
                                              .is_in_lookahead = false};
    sxml_regex_counts written_counts = {.instruction_count = 0};
    status = sxml_regex_compile(configuration, new_regex, &written_counts);
    if (status != SXML_STATUS_OK) {
        /* The counting pass succeeded on the same text, so this is not
           reached; reported rather than assumed. */
        return status;
    }
    *regex = new_regex;
    return SXML_STATUS_OK;
}

void sxml_regex_finalize(sxml_regex *regex) {
    /* Everything lives in the caller's block; nothing to release. */
    (void)regex;
}

sxml_status sxml_regex_create(sxml_allocator allocator,
                              const sxml_regex_configuration *configuration,
                              sxml_regex **regex) {
    sxml_regex_configuration compiling_configuration = *configuration;
    if (sxml_allocator_is_null(compiling_configuration.stack_allocator)) {
        compiling_configuration.stack_allocator = allocator;
    }
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_regex_memory_requirements(
        &compiling_configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex *new_regex = NULL;
    status = sxml_regex_initialize(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        &compiling_configuration, &new_regex);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_regex->owning_allocator = allocator;
    *regex = new_regex;
    return SXML_STATUS_OK;
}

void sxml_regex_destroy(sxml_regex *regex) {
    if (regex == NULL) {
        return;
    }
    sxml_allocator owning_allocator = regex->owning_allocator;
    sxml_regex_finalize(regex);
    sxml_allocator_release(owning_allocator, regex);
}

uint32_t sxml_regex_group_count(const sxml_regex *regex) {
    return regex->group_count;
}

sxml_regex_flags sxml_regex_get_flags(const sxml_regex *regex) {
    return regex->flags;
}

sxml_byte_view sxml_regex_group_name(const sxml_regex *regex,
                                     uint32_t group_number) {
    if (group_number > regex->group_count ||
        regex->groups[group_number].name_byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    const sxml_regex_group *group = &regex->groups[group_number];
    return (sxml_byte_view){.bytes = regex->group_names + group->name_offset,
                            .byte_count = group->name_byte_count};
}

uint32_t sxml_regex_find_named_group(const sxml_regex *regex,
                                     sxml_byte_view name) {
    for (uint32_t group_number = 1; group_number <= regex->group_count;
         group_number++) {
        sxml_byte_view group_name = sxml_regex_group_name(regex, group_number);
        if (group_name.byte_count != 0 &&
            group_name.byte_count == name.byte_count &&
            memcmp(group_name.bytes, name.bytes, name.byte_count) == 0) {
            return group_number;
        }
    }
    return 0;
}

uint32_t sxml_regex_group_parent(const sxml_regex *regex,
                                 uint32_t group_number) {
    return group_number <= regex->group_count
               ? regex->groups[group_number].parent_group_number
               : 0;
}

bool sxml_regex_group_is_in_lookahead(const sxml_regex *regex,
                                      uint32_t group_number) {
    return group_number <= regex->group_count &&
           regex->groups[group_number].is_in_lookahead;
}

/* ------------------------------------------------------------- Matching */

typedef enum sxml_regex_backtrack_kind {
    /* Resume at instruction index and position byte_offset. */
    SXML_REGEX_BACKTRACK_KIND_RESUME,
    /* Put byte_offset back into slot index. */
    SXML_REGEX_BACKTRACK_KIND_RESTORE_SLOT,
    /* The start of a positive assertion at byte_offset: reaching it when
       backtracking means the assertion's regex failed, so the assertion
       fails too. */
    SXML_REGEX_BACKTRACK_KIND_POSITIVE_ASSERTION,
    /* The start of a negative assertion at byte_offset: reaching it when
       backtracking means the assertion holds, and matching resumes at
       instruction index (after its LOOK_END) and byte_offset. */
    SXML_REGEX_BACKTRACK_KIND_NEGATIVE_ASSERTION,
} sxml_regex_backtrack_kind;

typedef struct sxml_regex_backtrack_entry {
    sxml_regex_backtrack_kind kind;
    uint32_t index;
    size_t byte_offset;
} sxml_regex_backtrack_entry;

/* A slot that holds no position. */
#define SXML_REGEX_UNSET_SLOT SIZE_MAX

struct sxml_regex_matcher {
    const sxml_regex *regex;
    sxml_allocator backtrack_allocator;
    /* The allocator of sxml_regex_matcher_create; null otherwise. */
    sxml_allocator owning_allocator;
    size_t maximum_backtrack_entry_count;
    uint64_t maximum_step_count;
    uint64_t step_count;
    /* regex->slot_count positions. */
    size_t *slots;
    /* The entries placed with the matcher. */
    sxml_regex_backtrack_entry *initial_entries;
    size_t initial_entry_count;
    /* The stack in use: initial_entries, or a block of the allocator. */
    sxml_regex_backtrack_entry *entries;
    size_t entry_capacity;
    size_t entry_count;
    sxml_string regex_input;
    bool has_searched;
    bool has_match;
    bool has_next_search;
    size_t next_search_byte_offset;
};

/* The memory of a matcher: header, slots, initial entries. */
typedef struct sxml_regex_matcher_layout {
    sxml_memory_requirements requirements;
    size_t slots_offset;
    size_t entries_offset;
    size_t initial_entry_count;
} sxml_regex_matcher_layout;

static sxml_status sxml_regex_matcher_compute_layout(
    const sxml_regex_matcher_configuration *configuration,
    sxml_regex_matcher_layout *layout) {
    if (configuration->regex == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_regex_matcher_layout new_layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
        .initial_entry_count =
            configuration->initial_backtrack_entry_count == 0
                ? SXML_REGEX_DEFAULT_INITIAL_BACKTRACK_ENTRY_COUNT
                : configuration->initial_backtrack_entry_count,
    };
    size_t header_offset = 0;
    sxml_status status = sxml_memory_requirements_append(
        &new_layout.requirements,
        (sxml_memory_requirements){
            .byte_count = sizeof(struct sxml_regex_matcher),
            .alignment = alignof(struct sxml_regex_matcher)},
        &header_offset);
    size_t array_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(configuration->regex->slot_count,
                                    sizeof(size_t), &array_byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){.byte_count = array_byte_count,
                                       .alignment = alignof(size_t)},
            &new_layout.slots_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_layout.initial_entry_count,
                                    sizeof(sxml_regex_backtrack_entry),
                                    &array_byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &new_layout.requirements,
            (sxml_memory_requirements){
                .byte_count = array_byte_count,
                .alignment = alignof(sxml_regex_backtrack_entry)},
            &new_layout.entries_offset);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = new_layout;
    return SXML_STATUS_OK;
}

sxml_status sxml_regex_matcher_memory_requirements(
    const sxml_regex_matcher_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_regex_matcher_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1}};
    sxml_status status =
        sxml_regex_matcher_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

sxml_status sxml_regex_matcher_initialize(
    sxml_memory_view memory,
    const sxml_regex_matcher_configuration *configuration,
    sxml_regex_matcher **matcher) {
    sxml_regex_matcher_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1}};
    sxml_status status =
        sxml_regex_matcher_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, layout.requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t maximum_backtrack_byte_count =
        configuration->maximum_backtrack_byte_count == 0
            ? SXML_REGEX_DEFAULT_MAXIMUM_BACKTRACK_BYTE_COUNT
            : configuration->maximum_backtrack_byte_count;
    size_t maximum_entry_count =
        maximum_backtrack_byte_count / sizeof(sxml_regex_backtrack_entry);
    if (maximum_entry_count < layout.initial_entry_count) {
        maximum_entry_count = layout.initial_entry_count;
    }
    void *placement = memory.bytes;
    sxml_regex_matcher *new_matcher = placement;
    void *slots_placement = memory.bytes + layout.slots_offset;
    void *entries_placement = memory.bytes + layout.entries_offset;
    *new_matcher = (sxml_regex_matcher){
        .regex = configuration->regex,
        .backtrack_allocator = configuration->backtrack_allocator,
        .owning_allocator = {.state = NULL, .operations = NULL},
        .maximum_backtrack_entry_count = maximum_entry_count,
        .maximum_step_count = configuration->maximum_step_count == 0
                                  ? SXML_REGEX_DEFAULT_MAXIMUM_STEP_COUNT
                                  : configuration->maximum_step_count,
        .step_count = 0,
        .slots = slots_placement,
        .initial_entries = entries_placement,
        .initial_entry_count = layout.initial_entry_count,
        .entries = entries_placement,
        .entry_capacity = layout.initial_entry_count,
        .entry_count = 0,
        .regex_input = {.utf8 = {.bytes = NULL, .byte_count = 0},
                        .codepoint_count = 0,
                        .is_ascii = true},
        .has_searched = false,
        .has_match = false,
        .has_next_search = false,
        .next_search_byte_offset = 0,
    };
    *matcher = new_matcher;
    return SXML_STATUS_OK;
}

/* Gives a grown backtrack stack back to its allocator. */
static void sxml_regex_matcher_release_entries(sxml_regex_matcher *matcher) {
    if (matcher->entries != matcher->initial_entries) {
        sxml_allocator_release(matcher->backtrack_allocator, matcher->entries);
        matcher->entries = matcher->initial_entries;
        matcher->entry_capacity = matcher->initial_entry_count;
    }
}

void sxml_regex_matcher_finalize(sxml_regex_matcher *matcher) {
    if (matcher == NULL) {
        return;
    }
    sxml_regex_matcher_release_entries(matcher);
}

sxml_status sxml_regex_matcher_create(
    sxml_allocator allocator,
    const sxml_regex_matcher_configuration *configuration,
    sxml_regex_matcher **matcher) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_regex_matcher_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_matcher *new_matcher = NULL;
    status = sxml_regex_matcher_initialize(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        configuration, &new_matcher);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_matcher->owning_allocator = allocator;
    *matcher = new_matcher;
    return SXML_STATUS_OK;
}

void sxml_regex_matcher_destroy(sxml_regex_matcher *matcher) {
    if (matcher == NULL) {
        return;
    }
    sxml_allocator owning_allocator = matcher->owning_allocator;
    sxml_regex_matcher_finalize(matcher);
    sxml_allocator_release(owning_allocator, matcher);
}

/* Doubles the backtrack stack, within its limit. */
static sxml_status sxml_regex_matcher_grow_entries(
    sxml_regex_matcher *matcher) {
    if (matcher->entry_capacity >= matcher->maximum_backtrack_entry_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (sxml_allocator_is_null(matcher->backtrack_allocator)) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    size_t new_capacity = matcher->entry_capacity;
    if (sxml_size_multiply(new_capacity, 2, &new_capacity) != SXML_STATUS_OK ||
        new_capacity > matcher->maximum_backtrack_entry_count) {
        new_capacity = matcher->maximum_backtrack_entry_count;
    }
    if (new_capacity == 0) {
        new_capacity = 1;
    }
    /* new_capacity is at most maximum_backtrack_byte_count / entry size. */
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        matcher->backtrack_allocator,
        (sxml_allocation_request){
            .byte_count = new_capacity * sizeof(sxml_regex_backtrack_entry),
            .alignment = alignof(sxml_regex_backtrack_entry),
        },
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_backtrack_entry *new_entries = memory;
    if (matcher->entry_count != 0) {
        memcpy(new_entries, matcher->entries,
               matcher->entry_count * sizeof(sxml_regex_backtrack_entry));
    }
    sxml_regex_matcher_release_entries(matcher);
    matcher->entries = new_entries;
    matcher->entry_capacity = new_capacity;
    return SXML_STATUS_OK;
}

static sxml_status sxml_regex_matcher_push(sxml_regex_matcher *matcher,
                                           sxml_regex_backtrack_entry entry) {
    if (matcher->entry_count == matcher->entry_capacity) {
        sxml_status status = sxml_regex_matcher_grow_entries(matcher);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    matcher->entries[matcher->entry_count] = entry;
    matcher->entry_count += 1;
    return SXML_STATUS_OK;
}

/* XML 1.0 (fifth edition), section 2.3: NameStartChar, and the
   characters NameChar adds to it. */
static const sxml_regex_codepoint_range sxml_regex_name_start_ranges[] = {
    {':', ':'},         {'A', 'Z'},         {'_', '_'},
    {'a', 'z'},         {0xC0, 0xD6},       {0xD8, 0xF6},
    {0xF8, 0x2FF},      {0x370, 0x37D},     {0x37F, 0x1FFF},
    {0x200C, 0x200D},   {0x2070, 0x218F},   {0x2C00, 0x2FEF},
    {0x3001, 0xD7FF},   {0xF900, 0xFDCF},   {0xFDF0, 0xFFFD},
    {0x10000, 0xEFFFF},
};
static const sxml_regex_codepoint_range sxml_regex_name_extra_ranges[] = {
    {'-', '-'}, {'.', '.'}, {'0', '9'}, {0xB7, 0xB7},
    {0x300, 0x36F}, {0x203F, 0x2040},
};

typedef struct sxml_regex_range_list {
    const sxml_regex_codepoint_range *ranges;
    size_t range_count;
} sxml_regex_range_list;

static bool sxml_regex_range_list_contains(sxml_regex_range_list list,
                                           uint32_t codepoint) {
    for (size_t range_index = 0; range_index < list.range_count;
         range_index++) {
        if (codepoint >= list.ranges[range_index].first_codepoint &&
            codepoint <= list.ranges[range_index].last_codepoint) {
            return true;
        }
    }
    return false;
}

#define SXML_REGEX_RANGE_LIST(array)                                         \
    ((sxml_regex_range_list){.ranges = (array),                              \
                             .range_count = sizeof(array) / sizeof((array)[0])})

static bool sxml_regex_is_name_start_character(uint32_t codepoint) {
    return sxml_regex_range_list_contains(
        SXML_REGEX_RANGE_LIST(sxml_regex_name_start_ranges), codepoint);
}

static bool sxml_regex_is_name_character(uint32_t codepoint) {
    return sxml_regex_is_name_start_character(codepoint) ||
           sxml_regex_range_list_contains(
               SXML_REGEX_RANGE_LIST(sxml_regex_name_extra_ranges), codepoint);
}

/* The character a class is asked about, with its case variants, found
   the first time a case-insensitive range needs them (section 6.2). */
typedef struct sxml_regex_class_query {
    uint32_t codepoint;
    bool has_variants;
    sxml_regex_case_variants variants;
} sxml_regex_class_query;

/* True when a range under the i flag holds the character or one of its
   case variants. */
static bool sxml_regex_case_insensitive_range_contains(
    const sxml_regex_class_item *item, sxml_regex_class_query *query) {
    if (!query->has_variants) {
        sxml_regex_find_case_variants(query->codepoint, &query->variants);
        query->has_variants = true;
    }
    for (size_t variant_index = 0;
         variant_index < query->variants.codepoint_count; variant_index++) {
        uint32_t variant = query->variants.codepoints[variant_index];
        if (variant >= item->first_codepoint && variant <= item->last_codepoint) {
            return true;
        }
    }
    return false;
}

static bool sxml_regex_class_item_contains(const sxml_regex_class_item *item,
                                           sxml_regex_class_query *query) {
    uint32_t codepoint = query->codepoint;
    bool is_member = false;
    switch (item->kind) {
    case SXML_REGEX_CLASS_ITEM_KIND_RANGE:
        is_member = (codepoint >= item->first_codepoint &&
                     codepoint <= item->last_codepoint) ||
                    (item->is_case_insensitive &&
                     sxml_regex_case_insensitive_range_contains(item, query));
        break;
    case SXML_REGEX_CLASS_ITEM_KIND_SPACE:
        is_member = sxml_regex_is_removable_whitespace(codepoint);
        break;
    case SXML_REGEX_CLASS_ITEM_KIND_DECIMAL_DIGIT:
        is_member = sxml_regex_is_decimal_digit(codepoint);
        break;
    case SXML_REGEX_CLASS_ITEM_KIND_WORD_CHARACTER:
        is_member = sxml_regex_is_word_character(codepoint);
        break;
    case SXML_REGEX_CLASS_ITEM_KIND_NAME_START_CHARACTER:
        is_member = sxml_regex_is_name_start_character(codepoint);
        break;
    case SXML_REGEX_CLASS_ITEM_KIND_NAME_CHARACTER:
        is_member = sxml_regex_is_name_character(codepoint);
        break;
    case SXML_REGEX_CLASS_ITEM_KIND_CATEGORY:
        is_member = (item->category_mask &
                     ((uint32_t)1 << (uint32_t)sxml_regex_get_general_category(
                          codepoint))) != 0;
        break;
    }
    return is_member != item->is_complemented;
}

/* True when the character is in the class's own items, with its negation
   but without its subtraction. */
static bool sxml_regex_class_own_contains(const sxml_regex_class *regex_class,
                                          const sxml_regex *regex,
                                          sxml_regex_class_query *query) {
    bool is_member = false;
    for (uint32_t item_index = regex_class->first_item_index;
         item_index < regex_class->first_item_index + regex_class->item_count;
         item_index++) {
        if (sxml_regex_class_item_contains(&regex->class_items[item_index],
                                           query)) {
            is_member = true;
            break;
        }
    }
    return is_member != regex_class->is_negated;
}

/*
 * True when codepoint is in class class_index. A class with a subtraction
 * contains what its own items contain and the subtracted class does not
 * (XSD 1.1 Part 2, appendix G, charClassSub), so along the chain of
 * subtracted classes C0, C1, ... membership is own(C0) and not (own(C1)
 * and not (...)): a loop that stops at the first class whose own items
 * leave codepoint out, or at the end of the chain, and counts the
 * negations passed. Subtracted classes come later in the class array, so
 * the chain ends.
 */
static bool sxml_regex_class_contains(const sxml_regex *regex,
                                      uint32_t class_index,
                                      uint32_t codepoint) {
    sxml_regex_class_query query = {.codepoint = codepoint,
                                    .has_variants = false,
                                    .variants = {.codepoint_count = 0}};
    bool is_negated = false;
    const sxml_regex_class *regex_class = &regex->classes[class_index];
    for (;;) {
        if (!sxml_regex_class_own_contains(regex_class, regex, &query)) {
            return is_negated;
        }
        if (regex_class->subtracted_class_number == 0) {
            return !is_negated;
        }
        is_negated = !is_negated;
        regex_class = &regex->classes[regex_class->subtracted_class_number - 1];
    }
}

/* Reads the codepoint at *byte_offset and advances it; false at the end. */
static bool sxml_regex_matcher_read_codepoint(
    const sxml_regex_matcher *matcher, size_t *byte_offset,
    uint32_t *codepoint) {
    sxml_codepoint_iterator iterator = {.string = matcher->regex_input,
                                        .byte_offset = *byte_offset};
    if (!sxml_codepoint_iterator_next(&iterator, codepoint)) {
        return false;
    }
    *byte_offset = iterator.byte_offset;
    return true;
}

/* Section 6.1.8.1: with m, "^" matches after a newline that is not the
   last character, "$" before a newline or at the end unless the last
   character is a newline. */
static bool sxml_regex_matcher_is_at_line_start(
    const sxml_regex_matcher *matcher, size_t byte_offset) {
    const sxml_byte_view input = matcher->regex_input.utf8;
    return byte_offset == 0 || (input.bytes[byte_offset - 1] == '\n' &&
                                byte_offset != input.byte_count);
}

static bool sxml_regex_matcher_is_at_line_end(
    const sxml_regex_matcher *matcher, size_t byte_offset) {
    const sxml_byte_view input = matcher->regex_input.utf8;
    if (byte_offset == input.byte_count) {
        return input.byte_count == 0 ||
               input.bytes[input.byte_count - 1] != '\n';
    }
    return input.bytes[byte_offset] == '\n';
}

/* With the i flag a back-reference matches the captured characters one
   by one, each or a case variant of it (section 6.2). */
static bool sxml_regex_matcher_match_back_reference_case_blind(
    const sxml_regex_matcher *matcher, sxml_byte_range captured,
    size_t *byte_offset) {
    size_t captured_offset = captured.byte_offset;
    size_t input_offset = *byte_offset;
    while (captured_offset < captured.byte_offset + captured.byte_count) {
        uint32_t captured_codepoint = 0;
        uint32_t input_codepoint = 0;
        (void)sxml_regex_matcher_read_codepoint(matcher, &captured_offset,
                                                &captured_codepoint);
        if (!sxml_regex_matcher_read_codepoint(matcher, &input_offset,
                                               &input_codepoint) ||
            !sxml_regex_are_case_variants(captured_codepoint,
                                          input_codepoint)) {
            return false;
        }
    }
    *byte_offset = input_offset;
    return true;
}

/* Section 6.1.6: a back-reference matches what the group captured, or the
   empty string when the group has captured nothing. */
static bool sxml_regex_matcher_match_back_reference(
    const sxml_regex_matcher *matcher, uint32_t group_number,
    size_t *byte_offset) {
    size_t group_start = matcher->slots[(size_t)2 * group_number];
    size_t group_end = matcher->slots[(size_t)2 * group_number + 1];
    if (group_start == SXML_REGEX_UNSET_SLOT ||
        group_end == SXML_REGEX_UNSET_SLOT || group_end < group_start) {
        return true;
    }
    size_t group_byte_count = group_end - group_start;
    if (matcher->regex->flags.is_case_insensitive) {
        return sxml_regex_matcher_match_back_reference_case_blind(
            matcher,
            (sxml_byte_range){.byte_offset = group_start,
                              .byte_count = group_byte_count},
            byte_offset);
    }
    const sxml_byte_view input = matcher->regex_input.utf8;
    if (group_byte_count > input.byte_count - *byte_offset) {
        return false;
    }
    if (group_byte_count != 0 &&
        memcmp(input.bytes + group_start, input.bytes + *byte_offset,
               group_byte_count) != 0) {
        return false;
    }
    *byte_offset += group_byte_count;
    return true;
}

/* The offset of the codepoint before the one at byte_offset (not 0). */
static size_t sxml_regex_previous_codepoint_offset(sxml_string input,
                                                   size_t byte_offset) {
    size_t previous_offset = byte_offset - 1;
    while (previous_offset > 0 &&
           (input.utf8.bytes[previous_offset] & 0xC0U) == 0x80U) {
        previous_offset -= 1;
    }
    return previous_offset;
}

/* Section 6.1.8.2: \b matches where a \w character and a \W character
   (or the start or end of the string) meet. */
static bool sxml_regex_matcher_is_at_word_boundary(
    const sxml_regex_matcher *matcher, size_t byte_offset) {
    bool is_word_before = false;
    bool is_word_after = false;
    uint32_t codepoint = 0;
    if (byte_offset > 0) {
        size_t previous_offset = sxml_regex_previous_codepoint_offset(
            matcher->regex_input, byte_offset);
        (void)sxml_regex_matcher_read_codepoint(matcher, &previous_offset,
                                                &codepoint);
        is_word_before = sxml_regex_is_word_character(codepoint);
    }
    size_t next_offset = byte_offset;
    if (sxml_regex_matcher_read_codepoint(matcher, &next_offset, &codepoint)) {
        is_word_after = sxml_regex_is_word_character(codepoint);
    }
    return is_word_before != is_word_after;
}

/* Moves *byte_offset back codepoint_count codepoints; false when the
   input starts before that (a lookbehind branch longer than what
   precedes). */
static bool sxml_regex_matcher_step_back(const sxml_regex_matcher *matcher,
                                         uint32_t codepoint_count,
                                         size_t *byte_offset) {
    size_t offset = *byte_offset;
    for (uint32_t stepped_count = 0; stepped_count < codepoint_count;
         stepped_count++) {
        if (offset == 0) {
            return false;
        }
        offset = sxml_regex_previous_codepoint_offset(matcher->regex_input,
                                                      offset);
    }
    *byte_offset = offset;
    return true;
}

/* The index of the innermost assertion's entry on the backtrack stack;
   LOOK_END runs only inside an assertion, so there is one. */
static size_t sxml_regex_matcher_find_assertion_entry(
    const sxml_regex_matcher *matcher) {
    size_t entry_index = matcher->entry_count;
    while (entry_index > 0) {
        entry_index -= 1;
        sxml_regex_backtrack_kind kind = matcher->entries[entry_index].kind;
        if (kind == SXML_REGEX_BACKTRACK_KIND_POSITIVE_ASSERTION ||
            kind == SXML_REGEX_BACKTRACK_KIND_NEGATIVE_ASSERTION) {
            return entry_index;
        }
    }
    return 0;
}

/*
 * LOOK_END: the regex of the innermost assertion has matched (sections
 * 6.1.8.3 to 6.1.8.6). A positive assertion holds: its choices are
 * dropped, so that no other way of matching it is tried (the groups it
 * captured are those of its first match), but the entries that restore
 * slots stay, so backtracking past the assertion still undoes its
 * captures; matching goes on at the assertion's position. A negative
 * assertion fails: its entries are undone and the thread backtracks.
 * Returns false when the thread must backtrack.
 */
static bool sxml_regex_matcher_end_assertion(sxml_regex_matcher *matcher,
                                             size_t *byte_offset) {
    size_t assertion_index = sxml_regex_matcher_find_assertion_entry(matcher);
    const sxml_regex_backtrack_entry assertion =
        matcher->entries[assertion_index];
    if (assertion.kind == SXML_REGEX_BACKTRACK_KIND_NEGATIVE_ASSERTION) {
        while (matcher->entry_count > assertion_index + 1) {
            matcher->entry_count -= 1;
            const sxml_regex_backtrack_entry entry =
                matcher->entries[matcher->entry_count];
            if (entry.kind == SXML_REGEX_BACKTRACK_KIND_RESTORE_SLOT) {
                matcher->slots[entry.index] = entry.byte_offset;
            }
        }
        matcher->entry_count = assertion_index;
        return false;
    }
    size_t kept_count = assertion_index;
    for (size_t entry_index = assertion_index + 1;
         entry_index < matcher->entry_count; entry_index++) {
        if (matcher->entries[entry_index].kind ==
            SXML_REGEX_BACKTRACK_KIND_RESTORE_SLOT) {
            matcher->entries[kept_count] = matcher->entries[entry_index];
            kept_count += 1;
        }
    }
    matcher->entry_count = kept_count;
    *byte_offset = assertion.byte_offset;
    return true;
}

/* The state of one anchored attempt. */
typedef struct sxml_regex_thread {
    uint32_t instruction_index;
    size_t byte_offset;
    /* Set by a step when the thread cannot go on and must backtrack. */
    bool has_failed;
} sxml_regex_thread;

/* Sets a slot, pushing the entry that restores it when backtracking. */
static sxml_status sxml_regex_matcher_set_slot(sxml_regex_matcher *matcher,
                                               uint32_t slot_index,
                                               size_t value) {
    sxml_status status = sxml_regex_matcher_push(
        matcher, (sxml_regex_backtrack_entry){
                     .kind = SXML_REGEX_BACKTRACK_KIND_RESTORE_SLOT,
                     .index = slot_index,
                     .byte_offset = matcher->slots[slot_index]});
    if (status == SXML_STATUS_OK) {
        matcher->slots[slot_index] = value;
    }
    return status;
}

/*
 * COUNTED_LOOP_CHOOSE of loop (D147): whether thread, at the
 * instruction after it, goes out of the loop (that instruction, the
 * jump out) or into another iteration (the one after), and the other way
 * as a choice while both are allowed (section 6.1.4).
 */
static sxml_status sxml_regex_matcher_choose_iteration(
    sxml_regex_matcher *matcher, const sxml_regex_counted_loop *loop,
    sxml_regex_thread *thread) {
    size_t counter = matcher->slots[loop->counter_slot];
    uint32_t out_index = thread->instruction_index;
    uint32_t iteration_index = out_index + 1;
    if (counter < loop->minimum_count) {
        thread->instruction_index = iteration_index;
        return SXML_STATUS_OK;
    }
    if (!loop->is_unbounded && counter >= loop->maximum_count) {
        return SXML_STATUS_OK;
    }
    thread->instruction_index =
        loop->is_reluctant ? out_index : iteration_index;
    return sxml_regex_matcher_push(
        matcher, (sxml_regex_backtrack_entry){
                     .kind = SXML_REGEX_BACKTRACK_KIND_RESUME,
                     .index = loop->is_reluctant ? iteration_index : out_index,
                     .byte_offset = thread->byte_offset});
}

/*
 * COUNTED_LOOP_END of loop (D147): counts the iteration that ended. One
 * that consumed nothing fails at or above the minimum, as CHECK_PROGRESS
 * does in an unbounded loop; below it, it counts as all the iterations
 * the minimum still needs, since each would again match nothing where it
 * starts (F&O 4.0, section 6.1.1 picks a segment, not a way of matching
 * it, and section 6.1.5 leaves the captured groups of a repetition that
 * matched a zero-length string implementation-dependent).
 */
static sxml_status sxml_regex_matcher_end_iteration(
    sxml_regex_matcher *matcher, const sxml_regex_counted_loop *loop,
    sxml_regex_thread *thread) {
    size_t counter = matcher->slots[loop->counter_slot];
    size_t new_counter = counter + 1;
    if (matcher->slots[loop->progress_slot] == thread->byte_offset) {
        if (counter >= loop->minimum_count) {
            thread->has_failed = true;
            return SXML_STATUS_OK;
        }
        new_counter = loop->minimum_count;
    }
    return sxml_regex_matcher_set_slot(matcher, loop->counter_slot,
                                       new_counter);
}

/*
 * Runs one instruction of thread (in/out). Returns a failure status only
 * for limits and allocation.
 */
static sxml_status sxml_regex_matcher_step(sxml_regex_matcher *matcher,
                                           sxml_regex_thread *thread) {
    const sxml_regex_instruction instruction =
        matcher->regex->instructions[thread->instruction_index];
    uint32_t next_index = thread->instruction_index + 1;
    uint32_t codepoint = 0;
    bool is_successful = true;
    sxml_status status = SXML_STATUS_OK;
    switch (instruction.operation) {
    case SXML_REGEX_OPERATION_CODEPOINT:
        is_successful = sxml_regex_matcher_read_codepoint(
                            matcher, &thread->byte_offset, &codepoint) &&
                        codepoint == instruction.operand;
        break;
    case SXML_REGEX_OPERATION_CLASS:
        is_successful = sxml_regex_matcher_read_codepoint(
                            matcher, &thread->byte_offset, &codepoint) &&
                        sxml_regex_class_contains(matcher->regex,
                                                  instruction.operand,
                                                  codepoint);
        break;
    case SXML_REGEX_OPERATION_ANY_BUT_NEWLINE:
        is_successful = sxml_regex_matcher_read_codepoint(
                            matcher, &thread->byte_offset, &codepoint) &&
                        codepoint != '\n' && codepoint != '\r';
        break;
    case SXML_REGEX_OPERATION_ANY:
        is_successful = sxml_regex_matcher_read_codepoint(
            matcher, &thread->byte_offset, &codepoint);
        break;
    case SXML_REGEX_OPERATION_SPLIT_NEXT_FIRST:
        status = sxml_regex_matcher_push(
            matcher, (sxml_regex_backtrack_entry){
                         .kind = SXML_REGEX_BACKTRACK_KIND_RESUME,
                         .index = instruction.operand,
                         .byte_offset = thread->byte_offset});
        break;
    case SXML_REGEX_OPERATION_SPLIT_TARGET_FIRST:
        status = sxml_regex_matcher_push(
            matcher, (sxml_regex_backtrack_entry){
                         .kind = SXML_REGEX_BACKTRACK_KIND_RESUME,
                         .index = next_index,
                         .byte_offset = thread->byte_offset});
        next_index = instruction.operand;
        break;
    case SXML_REGEX_OPERATION_JUMP:
        next_index = instruction.operand;
        break;
    case SXML_REGEX_OPERATION_SAVE:
        status = sxml_regex_matcher_set_slot(matcher, instruction.operand,
                                             thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_CHECK_PROGRESS:
        is_successful =
            matcher->slots[instruction.operand] != thread->byte_offset;
        break;
    case SXML_REGEX_OPERATION_STRING_START:
        is_successful = thread->byte_offset == 0;
        break;
    case SXML_REGEX_OPERATION_STRING_END:
        is_successful =
            thread->byte_offset == matcher->regex_input.utf8.byte_count;
        break;
    case SXML_REGEX_OPERATION_LINE_START:
        is_successful =
            sxml_regex_matcher_is_at_line_start(matcher, thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_LINE_END:
        is_successful =
            sxml_regex_matcher_is_at_line_end(matcher, thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_BACK_REFERENCE:
        is_successful = sxml_regex_matcher_match_back_reference(
            matcher, instruction.operand, &thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_WORD_BOUNDARY:
        is_successful = sxml_regex_matcher_is_at_word_boundary(
            matcher, thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_NOT_WORD_BOUNDARY:
        is_successful = !sxml_regex_matcher_is_at_word_boundary(
            matcher, thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_POSITIVE_LOOK_START:
    case SXML_REGEX_OPERATION_NEGATIVE_LOOK_START:
        status = sxml_regex_matcher_push(
            matcher,
            (sxml_regex_backtrack_entry){
                .kind = instruction.operation ==
                                SXML_REGEX_OPERATION_POSITIVE_LOOK_START
                            ? SXML_REGEX_BACKTRACK_KIND_POSITIVE_ASSERTION
                            : SXML_REGEX_BACKTRACK_KIND_NEGATIVE_ASSERTION,
                .index = instruction.operand,
                .byte_offset = thread->byte_offset});
        break;
    case SXML_REGEX_OPERATION_LOOK_END:
        is_successful =
            sxml_regex_matcher_end_assertion(matcher, &thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_STEP_BACK:
        is_successful = sxml_regex_matcher_step_back(
            matcher, instruction.operand, &thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_COUNTED_LOOP_START:
        status = sxml_regex_matcher_set_slot(
            matcher,
            matcher->regex->counted_loops[instruction.operand].counter_slot,
            0);
        break;
    case SXML_REGEX_OPERATION_COUNTED_LOOP_CHOOSE:
        thread->instruction_index = next_index;
        status = sxml_regex_matcher_choose_iteration(
            matcher, &matcher->regex->counted_loops[instruction.operand],
            thread);
        next_index = thread->instruction_index;
        break;
    case SXML_REGEX_OPERATION_COUNTED_LOOP_BODY:
        status = sxml_regex_matcher_set_slot(
            matcher,
            matcher->regex->counted_loops[instruction.operand].progress_slot,
            thread->byte_offset);
        break;
    case SXML_REGEX_OPERATION_COUNTED_LOOP_END:
        thread->has_failed = false;
        status = sxml_regex_matcher_end_iteration(
            matcher,
            &matcher->regex->counted_loops
                 [matcher->regex->instructions[instruction.operand].operand],
            thread);
        is_successful = !thread->has_failed;
        next_index = instruction.operand;
        break;
    case SXML_REGEX_OPERATION_MATCH:
        /* The caller stops before running MATCH. */
        break;
    }
    thread->instruction_index = next_index;
    thread->has_failed = !is_successful;
    return status;
}

/* Pops the backtrack stack up to the next choice; false when none is
   left. */
static bool sxml_regex_matcher_backtrack(sxml_regex_matcher *matcher,
                                         sxml_regex_thread *thread) {
    while (matcher->entry_count != 0) {
        matcher->entry_count -= 1;
        const sxml_regex_backtrack_entry entry =
            matcher->entries[matcher->entry_count];
        switch (entry.kind) {
        case SXML_REGEX_BACKTRACK_KIND_RESTORE_SLOT:
            matcher->slots[entry.index] = entry.byte_offset;
            break;
        case SXML_REGEX_BACKTRACK_KIND_POSITIVE_ASSERTION:
            /* The assertion's regex failed: so does the assertion. */
            break;
        case SXML_REGEX_BACKTRACK_KIND_RESUME:
        case SXML_REGEX_BACKTRACK_KIND_NEGATIVE_ASSERTION:
            thread->instruction_index = entry.index;
            thread->byte_offset = entry.byte_offset;
            return true;
        }
    }
    return false;
}

/* The result of a search that found nothing. */
static const sxml_regex_search_result sxml_regex_nothing_found = {
    .is_found = false,
    .matching_segment = {.byte_offset = 0, .byte_count = 0},
};

/* The result of a search that found the segment in slots 0 and 1. */
static sxml_regex_search_result sxml_regex_matcher_read_found_result(
    const sxml_regex_matcher *matcher) {
    return (sxml_regex_search_result){
        .is_found = true,
        .matching_segment = {.byte_offset = matcher->slots[0],
                             .byte_count = matcher->slots[1] - matcher->slots[0]},
    };
}

/* One anchored attempt at start_byte_offset. */
static sxml_status sxml_regex_matcher_attempt(
    sxml_regex_matcher *matcher, size_t start_byte_offset,
    sxml_regex_search_result *result) {
    for (uint32_t slot_index = 0; slot_index < matcher->regex->slot_count;
         slot_index++) {
        matcher->slots[slot_index] = SXML_REGEX_UNSET_SLOT;
    }
    matcher->entry_count = 0;
    sxml_regex_thread thread = {.instruction_index = 0,
                                .byte_offset = start_byte_offset,
                                .has_failed = false};
    for (;;) {
        if (matcher->step_count >= matcher->maximum_step_count) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        matcher->step_count += 1;
        if (matcher->regex->instructions[thread.instruction_index].operation ==
            SXML_REGEX_OPERATION_MATCH) {
            matcher->slots[0] = start_byte_offset;
            matcher->slots[1] = thread.byte_offset;
            *result = sxml_regex_matcher_read_found_result(matcher);
            return SXML_STATUS_OK;
        }
        sxml_status status = sxml_regex_matcher_step(matcher, &thread);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (thread.has_failed &&
            !sxml_regex_matcher_backtrack(matcher, &thread)) {
            *result = sxml_regex_nothing_found;
            return SXML_STATUS_OK;
        }
    }
}

/* True when byte_offset is a codepoint boundary of the input. */
static bool sxml_regex_is_codepoint_boundary(sxml_string input,
                                             size_t byte_offset) {
    if (byte_offset > input.utf8.byte_count) {
        return false;
    }
    /* Continuation bytes are 10xxxxxx. */
    return byte_offset == input.utf8.byte_count ||
           (input.utf8.bytes[byte_offset] & 0xC0U) != 0x80U;
}

/* The offset of the codepoint after the one at byte_offset (not the end). */
static size_t sxml_regex_next_codepoint_offset(sxml_string input,
                                               size_t byte_offset) {
    size_t next_offset = byte_offset + 1;
    while (next_offset < input.utf8.byte_count &&
           (input.utf8.bytes[next_offset] & 0xC0U) == 0x80U) {
        next_offset += 1;
    }
    return next_offset;
}

/* Records the outcome of a search: the match, and where find_next goes on
   (F&O 4.0, section 6.1.1, disjoint matching segments). */
static void sxml_regex_matcher_record_search(
    sxml_regex_matcher *matcher, sxml_regex_search_result result) {
    matcher->has_match = result.is_found;
    matcher->has_next_search = false;
    if (!result.is_found) {
        return;
    }
    size_t match_start = matcher->slots[0];
    size_t match_end = matcher->slots[1];
    if (match_end > match_start) {
        matcher->has_next_search = true;
        matcher->next_search_byte_offset = match_end;
    } else if (match_start < matcher->regex_input.utf8.byte_count) {
        matcher->has_next_search = true;
        matcher->next_search_byte_offset =
            sxml_regex_next_codepoint_offset(matcher->regex_input,
                                             match_start);
    }
}

/* Starts a search: checks the request and resets the per-search state. */
static sxml_status sxml_regex_matcher_start_search(
    sxml_regex_matcher *matcher, const sxml_regex_search_request *request) {
    if (!sxml_regex_is_codepoint_boundary(request->regex_input,
                                          request->start_byte_offset)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    matcher->regex_input = request->regex_input;
    matcher->has_searched = true;
    matcher->has_match = false;
    matcher->has_next_search = false;
    matcher->step_count = 0;
    return SXML_STATUS_OK;
}

sxml_status sxml_regex_matcher_match_at(
    sxml_regex_matcher *matcher, const sxml_regex_search_request *request,
    sxml_regex_search_result *result) {
    sxml_status status = sxml_regex_matcher_start_search(matcher, request);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_search_result attempt_result = {.is_found = false};
    status = sxml_regex_matcher_attempt(matcher, request->start_byte_offset,
                                        &attempt_result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_regex_matcher_record_search(matcher, attempt_result);
    *result = attempt_result;
    return SXML_STATUS_OK;
}

/* Tries each start position from start_byte_offset in turn. */
static sxml_status sxml_regex_matcher_scan(sxml_regex_matcher *matcher,
                                           size_t start_byte_offset,
                                           sxml_regex_search_result *result) {
    size_t byte_offset = start_byte_offset;
    for (;;) {
        sxml_regex_search_result attempt_result = {.is_found = false};
        sxml_status status =
            sxml_regex_matcher_attempt(matcher, byte_offset, &attempt_result);
        if (status != SXML_STATUS_OK) {
            matcher->has_match = false;
            matcher->has_next_search = false;
            return status;
        }
        if (attempt_result.is_found ||
            byte_offset == matcher->regex_input.utf8.byte_count) {
            sxml_regex_matcher_record_search(matcher, attempt_result);
            *result = attempt_result;
            return SXML_STATUS_OK;
        }
        byte_offset =
            sxml_regex_next_codepoint_offset(matcher->regex_input, byte_offset);
    }
}

sxml_status sxml_regex_matcher_find(sxml_regex_matcher *matcher,
                                    const sxml_regex_search_request *request,
                                    sxml_regex_search_result *result) {
    sxml_status status = sxml_regex_matcher_start_search(matcher, request);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_regex_matcher_scan(matcher, request->start_byte_offset,
                                   result);
}

sxml_status sxml_regex_matcher_find_next(sxml_regex_matcher *matcher,
                                         sxml_regex_search_result *result) {
    if (!matcher->has_searched) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!matcher->has_next_search) {
        matcher->has_match = false;
        *result = sxml_regex_nothing_found;
        return SXML_STATUS_OK;
    }
    matcher->step_count = 0;
    return sxml_regex_matcher_scan(matcher, matcher->next_search_byte_offset,
                                   result);
}

uint64_t sxml_regex_matcher_get_step_count(const sxml_regex_matcher *matcher) {
    return matcher->step_count;
}

sxml_status sxml_regex_matcher_get_captured_group(
    const sxml_regex_matcher *matcher, uint32_t group_number,
    sxml_regex_captured_group *captured_group) {
    if (!matcher->has_match) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (group_number > matcher->regex->group_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    size_t group_start = matcher->slots[2 * (size_t)group_number];
    size_t group_end = matcher->slots[2 * (size_t)group_number + 1];
    if (group_start == SXML_REGEX_UNSET_SLOT ||
        group_end == SXML_REGEX_UNSET_SLOT || group_end < group_start) {
        *captured_group = (sxml_regex_captured_group){
            .byte_range = {.byte_offset = 0, .byte_count = 0},
            .is_captured = false,
        };
        return SXML_STATUS_OK;
    }
    *captured_group = (sxml_regex_captured_group){
        .byte_range = {.byte_offset = group_start,
                       .byte_count = group_end - group_start},
        .is_captured = true,
    };
    return SXML_STATUS_OK;
}
