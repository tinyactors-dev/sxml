/*
 * The regular expression seed (src/regex_internal.h; docs/DECISIONS.md D48
 * and D53): a hand-written recursive-descent parser that compiles the
 * regex into instructions, and a backtracking matcher that runs them with
 * an explicit stack.
 *
 * Imitates the golden module src/memory.c (lifecycle, requirements by
 * sxml_memory_requirements_append, checked sizes). The syntax is that of
 * F&O 4.0, section 6.1.3 "Regular expression grammar", which builds on
 * XSD 1.1 Part 2, appendix G "Regular Expressions"; the subset and what
 * is rejected as unsupported are listed in D53.
 *
 * Compilation runs the parser twice over the same code: a counting pass
 * (regex NULL) that finds the exact number of instructions, classes and
 * class items, and an emitting pass that writes them into memory laid out
 * from those counts. Counted repetition is expanded by parsing the atom
 * again for each copy; the extent of an atom is found first by a lexical
 * scan, so nested quantifiers cost one scan per level, not a parse.
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
    /* Matches what group operand captured (section 6.1.6). */
    SXML_REGEX_OPERATION_BACK_REFERENCE,
    /* The whole regex has matched. */
    SXML_REGEX_OPERATION_MATCH,
} sxml_regex_operation;

typedef struct sxml_regex_instruction {
    sxml_regex_operation operation;
    uint32_t operand;
} sxml_regex_instruction;

typedef enum sxml_regex_class_item_kind {
    SXML_REGEX_CLASS_ITEM_KIND_RANGE,
    /* \s: space, tab, newline, carriage return. */
    SXML_REGEX_CLASS_ITEM_KIND_SPACE,
    /* \d. */
    SXML_REGEX_CLASS_ITEM_KIND_DECIMAL_DIGIT,
    /* \w. */
    SXML_REGEX_CLASS_ITEM_KIND_WORD_CHARACTER,
} sxml_regex_class_item_kind;

/* One part of a character group: a range or a multi-character escape. */
typedef struct sxml_regex_class_item {
    sxml_regex_class_item_kind kind;
    /* \S, \D, \W. */
    bool is_complemented;
    /* RANGE only. */
    uint32_t first_codepoint;
    uint32_t last_codepoint;
} sxml_regex_class_item;

/* A character class: items, negation, and an optional subtraction. */
typedef struct sxml_regex_class {
    uint32_t first_item_index;
    uint32_t item_count;
    bool is_negated;
    /* The class subtracted ([a-z-[aeiou]]), plus one; 0 for none. */
    uint32_t subtracted_class_number;
} sxml_regex_class;

struct sxml_regex {
    sxml_regex_flags flags;
    uint32_t group_count;
    /* Two slots per group, group 0 included, then one per progress
       register of an unbounded loop. */
    uint32_t slot_count;
    uint32_t instruction_count;
    uint32_t class_count;
    uint32_t class_item_count;
    sxml_regex_instruction *instructions;
    sxml_regex_class *classes;
    sxml_regex_class_item *class_items;
    /* The allocator of sxml_regex_create; the null allocator otherwise. */
    sxml_allocator owning_allocator;
};

/* What a compiling pass produced. */
typedef struct sxml_regex_counts {
    uint32_t instruction_count;
    uint32_t class_count;
    uint32_t class_item_count;
    uint32_t group_count;
    uint32_t register_count;
} sxml_regex_counts;

/* ------------------------------------------------------------- Compiler */

/* A capturing group whose ")" has not been read yet. */
typedef struct sxml_regex_open_group {
    uint32_t group_number;
    const struct sxml_regex_open_group *enclosing_group;
} sxml_regex_open_group;

typedef struct sxml_regex_compiler {
    sxml_string regex_text;
    size_t byte_offset;
    /* Inside [...], where x and c remove nothing (section 6.2). */
    bool is_in_class;
    sxml_regex_flags flags;
    /* The regex being written; NULL in the counting pass. */
    sxml_regex *regex;
    sxml_regex_counts counts;
    /* Groups of the whole regex, from the counting pass; progress
       registers are numbered after their slots. */
    uint32_t total_group_count;
    uint32_t maximum_instruction_count;
    uint32_t maximum_group_depth;
    uint32_t group_depth;
    /* Atoms parsed, copies included: bounds compile work even for atoms
       that emit nothing, as in (?:){1000}{1000}. */
    uint32_t atom_parse_count;
    const sxml_regex_open_group *innermost_open_group;
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
#define SXML_REGEX_UNSUPPORTED(compiler, text)                               \
    sxml_regex_syntax_error((compiler),                                       \
                            SXML_BYTE_VIEW_FROM_LITERAL(                      \
                                text " not supported by the regex seed "     \
                                     "(docs/DECISIONS.md, D53)"))

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

/*
 * Instructions whose operand is a target not known yet. Each pending
 * operand holds the next pending instruction plus one, so the list needs
 * no memory of its own.
 */
typedef struct sxml_regex_pending_targets {
    uint32_t last_instruction_number;
} sxml_regex_pending_targets;

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
                      .first_codepoint = range.first_codepoint,
                      .last_codepoint = range.last_codepoint,
                  });
}

/* Characters outside ASCII whose case variants include an ASCII letter. */
#define SXML_REGEX_KELVIN_SIGN 0x212AU
#define SXML_REGEX_LATIN_SMALL_LONG_S 0x017FU
#define SXML_REGEX_LATIN_SMALL_DOTLESS_I 0x0131U
#define SXML_REGEX_ASCII_LIMIT 0x80U
#define SXML_REGEX_ASCII_CASE_DISTANCE 32U

static bool sxml_regex_range_contains(sxml_regex_codepoint_range range,
                                      uint32_t codepoint) {
    return codepoint >= range.first_codepoint &&
           codepoint <= range.last_codepoint;
}

/* The letters of one ASCII case, and which way the other case lies. */
typedef struct sxml_regex_case_shift {
    sxml_regex_codepoint_range letters;
    bool is_upward;
} sxml_regex_case_shift;

/* Emits the part of range within shift.letters, moved to the other case. */
static sxml_status sxml_regex_compiler_emit_shifted_letters(
    sxml_regex_compiler *compiler, sxml_regex_codepoint_range range,
    sxml_regex_case_shift shift) {
    uint32_t first_codepoint = range.first_codepoint > shift.letters.first_codepoint
                                   ? range.first_codepoint
                                   : shift.letters.first_codepoint;
    uint32_t last_codepoint = range.last_codepoint < shift.letters.last_codepoint
                                  ? range.last_codepoint
                                  : shift.letters.last_codepoint;
    if (first_codepoint > last_codepoint) {
        return SXML_STATUS_OK;
    }
    sxml_regex_codepoint_range shifted = {
        .first_codepoint = shift.is_upward
                               ? first_codepoint + SXML_REGEX_ASCII_CASE_DISTANCE
                               : first_codepoint - SXML_REGEX_ASCII_CASE_DISTANCE,
        .last_codepoint = shift.is_upward
                              ? last_codepoint + SXML_REGEX_ASCII_CASE_DISTANCE
                              : last_codepoint - SXML_REGEX_ASCII_CASE_DISTANCE,
    };
    return sxml_regex_compiler_emit_range_item(compiler, shifted);
}

/*
 * Emits a range of a character group, with its case variants under the i
 * flag (F&O 4.0, section 6.2: C2 is a case variant of C1 when their
 * fn:lower-case or their fn:upper-case are equal). For ASCII letters these
 * are the other ASCII case plus three characters outside ASCII: KELVIN
 * SIGN (lower case k), LATIN SMALL LETTER LONG S (upper case S) and LATIN
 * SMALL LETTER DOTLESS I (upper case I). The seed has no case data beyond
 * that, so a range reaching outside ASCII is unsupported under i.
 */
static sxml_status sxml_regex_compiler_emit_range(
    sxml_regex_compiler *compiler, sxml_regex_codepoint_range range) {
    if (compiler->flags.is_case_insensitive &&
        range.last_codepoint >= SXML_REGEX_ASCII_LIMIT) {
        return SXML_REGEX_UNSUPPORTED(
            compiler, "Case-insensitive matching of non-ASCII characters is");
    }
    sxml_status status = sxml_regex_compiler_emit_range_item(compiler, range);
    if (status != SXML_STATUS_OK || !compiler->flags.is_case_insensitive) {
        return status;
    }
    status = sxml_regex_compiler_emit_shifted_letters(
        compiler, range,
        (sxml_regex_case_shift){
            .letters = {.first_codepoint = 'A', .last_codepoint = 'Z'},
            .is_upward = true,
        });
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_regex_compiler_emit_shifted_letters(
        compiler, range,
        (sxml_regex_case_shift){
            .letters = {.first_codepoint = 'a', .last_codepoint = 'z'},
            .is_upward = false,
        });
    if (status != SXML_STATUS_OK) {
        return status;
    }
    static const struct {
        uint32_t lower_case_letter;
        uint32_t other_variant;
    } outside_variants[] = {
        {'k', SXML_REGEX_KELVIN_SIGN},
        {'s', SXML_REGEX_LATIN_SMALL_LONG_S},
        {'i', SXML_REGEX_LATIN_SMALL_DOTLESS_I},
    };
    for (size_t variant_index = 0;
         variant_index < sizeof(outside_variants) / sizeof(outside_variants[0]);
         variant_index++) {
        uint32_t letter = outside_variants[variant_index].lower_case_letter;
        if (sxml_regex_range_contains(range, letter) ||
            sxml_regex_range_contains(range,
                                      letter - SXML_REGEX_ASCII_CASE_DISTANCE)) {
            uint32_t variant = outside_variants[variant_index].other_variant;
            status = sxml_regex_compiler_emit_range_item(
                compiler, (sxml_regex_codepoint_range){
                              .first_codepoint = variant,
                              .last_codepoint = variant,
                          });
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    return SXML_STATUS_OK;
}

/* Emits the instruction that matches class class_index. */
static sxml_status sxml_regex_compiler_emit_class_instruction(
    sxml_regex_compiler *compiler, uint32_t class_index) {
    return sxml_regex_compiler_emit_operation(
        compiler, SXML_REGEX_OPERATION_CLASS, class_index);
}

/* Emits a normal character as an atom (with its case variants under i). */
static sxml_status sxml_regex_compiler_emit_literal(
    sxml_regex_compiler *compiler, uint32_t codepoint) {
    bool is_ascii_letter = (codepoint >= 'A' && codepoint <= 'Z') ||
                           (codepoint >= 'a' && codepoint <= 'z');
    if (!compiler->flags.is_case_insensitive ||
        (!is_ascii_letter && codepoint < SXML_REGEX_ASCII_LIMIT)) {
        return sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_CODEPOINT, codepoint);
    }
    uint32_t class_index = 0;
    sxml_status status =
        sxml_regex_compiler_reserve_class(compiler, &class_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t first_item_index = compiler->counts.class_item_count;
    status = sxml_regex_compiler_emit_range(
        compiler, (sxml_regex_codepoint_range){.first_codepoint = codepoint,
                                               .last_codepoint = codepoint});
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

/*
 * The class item of a multi-character escape (\s \S \d \D \w \W), or false
 * when escaped_codepoint is not one the seed supports.
 */
static bool sxml_regex_multi_character_escape(uint32_t escaped_codepoint,
                                              sxml_regex_class_item *item) {
    sxml_regex_class_item_kind kind = SXML_REGEX_CLASS_ITEM_KIND_SPACE;
    switch (escaped_codepoint) {
    case 's':
    case 'S':
        kind = SXML_REGEX_CLASS_ITEM_KIND_SPACE;
        break;
    case 'd':
    case 'D':
        kind = SXML_REGEX_CLASS_ITEM_KIND_DECIMAL_DIGIT;
        break;
    case 'w':
    case 'W':
        kind = SXML_REGEX_CLASS_ITEM_KIND_WORD_CHARACTER;
        break;
    default:
        return false;
    }
    *item = (sxml_regex_class_item){
        .kind = kind,
        .is_complemented = escaped_codepoint == 'S' ||
                           escaped_codepoint == 'D' ||
                           escaped_codepoint == 'W',
        .first_codepoint = 0,
        .last_codepoint = 0,
    };
    return true;
}

/* Reports an escape that is valid in the dialect but not in the seed, or
   invalid altogether. */
static sxml_status sxml_regex_compiler_reject_escape(
    sxml_regex_compiler *compiler, uint32_t escaped_codepoint) {
    switch (escaped_codepoint) {
    case 'p':
    case 'P':
        return SXML_REGEX_UNSUPPORTED(compiler,
                                      "Category and block escapes (\\p, \\P) are");
    case 'i':
    case 'I':
    case 'c':
    case 'C':
        return SXML_REGEX_UNSUPPORTED(
            compiler, "Name character escapes (\\i, \\I, \\c, \\C) are");
    case 'b':
    case 'B':
        return SXML_REGEX_UNSUPPORTED(compiler,
                                      "Word boundaries (\\b, \\B) are");
    case 'k':
        return SXML_REGEX_UNSUPPORTED(compiler,
                                      "Named back-references (\\k) are");
    default:
        return SXML_REGEX_INVALID(compiler, "unknown escape");
    }
}

static sxml_status sxml_regex_compiler_parse_class_expression(
    sxml_regex_compiler *compiler, uint32_t *class_index);

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
        sxml_regex_class_item item = {.kind = SXML_REGEX_CLASS_ITEM_KIND_SPACE};
        if (sxml_regex_multi_character_escape(escaped_codepoint, &item)) {
            uint32_t following_codepoint = 0;
            uint32_t second_codepoint = 0;
            if (sxml_regex_compiler_peek_raw(compiler, &following_codepoint) &&
                following_codepoint == '-' &&
                sxml_regex_compiler_peek_second_raw(compiler,
                                                    &second_codepoint) &&
                second_codepoint != ']' && second_codepoint != '[') {
                return SXML_REGEX_INVALID(
                    compiler, "a range cannot start with a class escape");
            }
            return sxml_regex_compiler_emit_class_item(compiler, item);
        }
        if (!sxml_regex_single_character_escape(escaped_codepoint,
                                                &first_codepoint)) {
            return sxml_regex_compiler_reject_escape(compiler,
                                                     escaped_codepoint);
        }
    }
    uint32_t last_codepoint = first_codepoint;
    uint32_t following_codepoint = 0;
    uint32_t second_codepoint = 0;
    if (sxml_regex_compiler_peek_raw(compiler, &following_codepoint) &&
        following_codepoint == '-' &&
        sxml_regex_compiler_peek_second_raw(compiler, &second_codepoint) &&
        second_codepoint != ']' && second_codepoint != '[') {
        (void)sxml_regex_compiler_next_raw(compiler, &following_codepoint);
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

/*
 * Reads a character class expression after its "[" (XSD 1.1 Part 2,
 * appendix G, charClassExpr: an optional "^", one or more group parts, an
 * optional subtraction "-[...]", then "]"). A "-" is a literal only as the
 * first part or the last; elsewhere it must be escaped.
 */
static sxml_status sxml_regex_compiler_parse_class_expression(
    sxml_regex_compiler *compiler, uint32_t *class_index) {
    if (compiler->group_depth >= compiler->maximum_group_depth) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    uint32_t new_class_index = 0;
    sxml_status status =
        sxml_regex_compiler_reserve_class(compiler, &new_class_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool was_in_class = compiler->is_in_class;
    compiler->is_in_class = true;
    compiler->group_depth += 1;
    sxml_regex_class regex_class = {
        .first_item_index = compiler->counts.class_item_count,
        .item_count = 0,
        .is_negated = false,
        .subtracted_class_number = 0,
    };
    uint32_t codepoint = 0;
    if (sxml_regex_compiler_peek_raw(compiler, &codepoint) && codepoint == '^') {
        (void)sxml_regex_compiler_next_raw(compiler, &codepoint);
        regex_class.is_negated = true;
    }
    uint32_t part_count = 0;
    for (;;) {
        if (!sxml_regex_compiler_next_raw(compiler, &codepoint)) {
            status = SXML_REGEX_INVALID(compiler, "unterminated character class");
            break;
        }
        if (codepoint == ']') {
            if (part_count == 0) {
                status = SXML_REGEX_INVALID(compiler, "empty character class");
            }
            break;
        }
        uint32_t following_codepoint = 0;
        bool has_following = sxml_regex_compiler_peek_raw(compiler,
                                                          &following_codepoint);
        if (codepoint == '-' && has_following && following_codepoint == '[') {
            if (part_count == 0) {
                status = SXML_REGEX_INVALID(compiler,
                                            "subtraction from an empty class");
                break;
            }
            (void)sxml_regex_compiler_next_raw(compiler, &following_codepoint);
            regex_class.item_count = compiler->counts.class_item_count -
                                     regex_class.first_item_index;
            uint32_t subtracted_class_index = 0;
            status = sxml_regex_compiler_parse_class_expression(
                compiler, &subtracted_class_index);
            if (status != SXML_STATUS_OK) {
                break;
            }
            regex_class.subtracted_class_number = subtracted_class_index + 1;
            if (!sxml_regex_compiler_next_raw(compiler, &codepoint) ||
                codepoint != ']') {
                status = SXML_REGEX_INVALID(
                    compiler, "a subtraction must end the character class");
            }
            break;
        }
        if (codepoint == '-' && part_count != 0 &&
            !(has_following && following_codepoint == ']')) {
            status = SXML_REGEX_INVALID(compiler,
                                        "\"-\" must be escaped here");
            break;
        }
        status = sxml_regex_compiler_parse_group_part(compiler, codepoint);
        if (status != SXML_STATUS_OK) {
            break;
        }
        part_count += 1;
    }
    compiler->group_depth -= 1;
    compiler->is_in_class = was_in_class;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (regex_class.subtracted_class_number == 0) {
        regex_class.item_count =
            compiler->counts.class_item_count - regex_class.first_item_index;
    }
    sxml_regex_compiler_write_class(compiler, new_class_index, regex_class);
    *class_index = new_class_index;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Lexical scanning */

/* True when group_number is a group whose ")" has been read. */
static bool sxml_regex_compiler_group_is_closed(
    const sxml_regex_compiler *compiler, uint32_t group_number) {
    if (group_number == 0 || group_number > compiler->counts.group_count) {
        return false;
    }
    for (const sxml_regex_open_group *open_group =
             compiler->innermost_open_group;
         open_group != NULL; open_group = open_group->enclosing_group) {
        if (open_group->group_number == group_number) {
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

/* Moves past a character class expression after its "[", by brackets. */
static void sxml_regex_compiler_skip_class(sxml_regex_compiler *compiler) {
    bool was_in_class = compiler->is_in_class;
    compiler->is_in_class = true;
    size_t bracket_depth = 1;
    uint32_t codepoint = 0;
    while (bracket_depth != 0 &&
           sxml_regex_compiler_next_raw(compiler, &codepoint)) {
        if (codepoint == '\\') {
            (void)sxml_regex_compiler_next_raw(compiler, &codepoint);
        } else if (codepoint == '[') {
            bracket_depth += 1;
        } else if (codepoint == ']') {
            bracket_depth -= 1;
        }
    }
    compiler->is_in_class = was_in_class;
}

/* Moves past the codepoints after a backslash that belong to the escape. */
static void sxml_regex_compiler_skip_escape(sxml_regex_compiler *compiler) {
    uint32_t codepoint = 0;
    if (sxml_regex_compiler_next_escaped(compiler, &codepoint) &&
        codepoint >= '1' && codepoint <= '9') {
        (void)sxml_regex_compiler_read_back_reference(compiler, codepoint);
    }
}

/*
 * Moves past one atom without checking it: one codepoint, an escape, a
 * class or a parenthesized group. Used to find where an atom ends before
 * its quantifier is known, and to count alternatives. Errors are left to
 * the parse that follows.
 */
static void sxml_regex_compiler_skip_atom(sxml_regex_compiler *compiler) {
    uint32_t codepoint = 0;
    if (!sxml_regex_compiler_next(compiler, &codepoint)) {
        return;
    }
    if (codepoint == '\\') {
        sxml_regex_compiler_skip_escape(compiler);
    } else if (codepoint == '[') {
        sxml_regex_compiler_skip_class(compiler);
    } else if (codepoint == '(') {
        size_t parenthesis_depth = 1;
        while (parenthesis_depth != 0 &&
               sxml_regex_compiler_next(compiler, &codepoint)) {
            if (codepoint == '\\') {
                (void)sxml_regex_compiler_next_escaped(compiler, &codepoint);
            } else if (codepoint == '[') {
                sxml_regex_compiler_skip_class(compiler);
            } else if (codepoint == '(') {
                parenthesis_depth += 1;
            } else if (codepoint == ')') {
                parenthesis_depth -= 1;
            }
        }
    }
}

/* The number of branches from here to the ")" or end of this regExp. */
static uint32_t sxml_regex_compiler_count_branches(
    sxml_regex_compiler *compiler) {
    size_t saved_byte_offset = compiler->byte_offset;
    uint32_t branch_count = 1;
    uint32_t codepoint = 0;
    while (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint != ')') {
        if (codepoint == '|') {
            (void)sxml_regex_compiler_next(compiler, &codepoint);
            branch_count += 1;
        } else {
            sxml_regex_compiler_skip_atom(compiler);
        }
    }
    compiler->byte_offset = saved_byte_offset;
    return branch_count;
}

/* ---------------------------------------------------------------- Parsing */

static sxml_status sxml_regex_compiler_parse_alternation(
    sxml_regex_compiler *compiler);

/* Reads a group after its "(" (F&O 4.0, section 6.1.3, atom). */
static sxml_status sxml_regex_compiler_parse_group(
    sxml_regex_compiler *compiler) {
    if (compiler->group_depth >= compiler->maximum_group_depth) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    bool is_capturing = true;
    uint32_t codepoint = 0;
    if (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint == '?') {
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        if (!sxml_regex_compiler_next(compiler, &codepoint)) {
            return SXML_REGEX_INVALID(compiler, "unterminated group");
        }
        if (codepoint == '=' || codepoint == '!' || codepoint == '<') {
            uint32_t following_codepoint = 0;
            bool is_lookaround =
                codepoint != '<' ||
                (sxml_regex_compiler_peek(compiler, &following_codepoint) &&
                 (following_codepoint == '=' || following_codepoint == '!'));
            if (is_lookaround) {
                return SXML_REGEX_UNSUPPORTED(
                    compiler, "Lookahead and lookbehind assertions are");
            }
            return SXML_REGEX_UNSUPPORTED(compiler, "Named groups are");
        }
        if (codepoint != ':') {
            return SXML_REGEX_INVALID(compiler, "unknown \"(?\" construct");
        }
        is_capturing = false;
    } else if (sxml_regex_compiler_peek(compiler, &codepoint) &&
               codepoint == '*') {
        return SXML_REGEX_UNSUPPORTED(compiler, "\"(*\" assertions are");
    }
    uint32_t group_number = 0;
    sxml_status status = SXML_STATUS_OK;
    if (is_capturing) {
        group_number = compiler->counts.group_count + 1;
        compiler->counts.group_count = group_number;
        status = sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_SAVE, 2 * group_number);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_regex_open_group open_group = {
        .group_number = group_number,
        .enclosing_group = compiler->innermost_open_group,
    };
    compiler->innermost_open_group = &open_group;
    compiler->group_depth += 1;
    status = sxml_regex_compiler_parse_alternation(compiler);
    compiler->group_depth -= 1;
    compiler->innermost_open_group = open_group.enclosing_group;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_regex_compiler_next(compiler, &codepoint) || codepoint != ')') {
        return SXML_REGEX_INVALID(compiler, "\"(\" without \")\"");
    }
    if (!is_capturing) {
        return SXML_STATUS_OK;
    }
    return sxml_regex_compiler_emit_operation(
        compiler, SXML_REGEX_OPERATION_SAVE, 2 * group_number + 1);
}

/* Reads an escape outside a class after its backslash. */
static sxml_status sxml_regex_compiler_parse_escape(
    sxml_regex_compiler *compiler) {
    uint32_t escaped_codepoint = 0;
    if (!sxml_regex_compiler_next_escaped(compiler, &escaped_codepoint)) {
        return SXML_REGEX_INVALID(compiler, "\"\\\" at the end");
    }
    uint32_t codepoint = 0;
    if (sxml_regex_single_character_escape(escaped_codepoint, &codepoint)) {
        return sxml_regex_compiler_emit_literal(compiler, codepoint);
    }
    sxml_regex_class_item item = {.kind = SXML_REGEX_CLASS_ITEM_KIND_SPACE};
    if (sxml_regex_multi_character_escape(escaped_codepoint, &item)) {
        uint32_t class_index = 0;
        sxml_status status =
            sxml_regex_compiler_reserve_class(compiler, &class_index);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint32_t first_item_index = compiler->counts.class_item_count;
        status = sxml_regex_compiler_emit_class_item(compiler, item);
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
    if (escaped_codepoint >= '1' && escaped_codepoint <= '9') {
        uint32_t group_number = sxml_regex_compiler_read_back_reference(
            compiler, escaped_codepoint);
        if (!sxml_regex_compiler_group_is_closed(compiler, group_number)) {
            return SXML_REGEX_INVALID(
                compiler,
                "a back-reference must follow the \")\" of its group");
        }
        if (compiler->flags.is_case_insensitive) {
            return SXML_REGEX_UNSUPPORTED(
                compiler, "Back-references with the i flag are");
        }
        return sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_BACK_REFERENCE, group_number);
    }
    return sxml_regex_compiler_reject_escape(compiler, escaped_codepoint);
}

/* Reads one atom (F&O 4.0, section 6.1.3, atom) and emits it. */
static sxml_status sxml_regex_compiler_parse_atom(
    sxml_regex_compiler *compiler) {
    if (compiler->atom_parse_count >= compiler->maximum_instruction_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    compiler->atom_parse_count += 1;
    uint32_t codepoint = 0;
    if (!sxml_regex_compiler_next(compiler, &codepoint)) {
        return SXML_REGEX_INVALID(compiler, "missing atom");
    }
    switch (codepoint) {
    case '(':
        return sxml_regex_compiler_parse_group(compiler);
    case '[': {
        uint32_t class_index = 0;
        sxml_status status =
            sxml_regex_compiler_parse_class_expression(compiler, &class_index);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_regex_compiler_emit_class_instruction(compiler,
                                                          class_index);
    }
    case '.':
        return sxml_regex_compiler_emit_operation(
            compiler,
            compiler->flags.is_dot_all ? SXML_REGEX_OPERATION_ANY
                                       : SXML_REGEX_OPERATION_ANY_BUT_NEWLINE,
            0);
    case '\\':
        return sxml_regex_compiler_parse_escape(compiler);
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
        return sxml_regex_compiler_emit_literal(compiler, codepoint);
    }
}

typedef struct sxml_regex_quantifier {
    /* False when the atom has no quantifier. */
    bool is_present;
    uint32_t minimum_count;
    uint32_t maximum_count;
    bool is_unbounded;
    bool is_reluctant;
} sxml_regex_quantifier;

/* Reads the digits of a quantity; values above UINT32_MAX saturate, and
   the instruction limit rejects them. */
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
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        quantifier->is_reluctant = true;
    }
    quantifier->is_present = true;
    return SXML_STATUS_OK;
}

/* Where an atom is in the regex text, so that it can be parsed again for
   each copy a quantifier needs. */
typedef struct sxml_regex_atom_extent {
    size_t start_byte_offset;
    size_t end_byte_offset;
    /* Groups opened before the atom: every copy numbers its groups alike. */
    uint32_t group_count_before;
} sxml_regex_atom_extent;

static sxml_status sxml_regex_compiler_emit_atom_copy(
    sxml_regex_compiler *compiler, sxml_regex_atom_extent extent) {
    compiler->byte_offset = extent.start_byte_offset;
    compiler->counts.group_count = extent.group_count_before;
    sxml_status status = sxml_regex_compiler_parse_atom(compiler);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (compiler->byte_offset != extent.end_byte_offset) {
        return SXML_REGEX_INVALID(compiler, "malformed atom");
    }
    return SXML_STATUS_OK;
}

/* Emits an atom with a quantifier: the minimum number of copies, then a
   loop or the optional copies (section 6.1.4 for reluctance). */
static sxml_status sxml_regex_compiler_emit_quantified_atom(
    sxml_regex_compiler *compiler, sxml_regex_atom_extent extent,
    sxml_regex_quantifier quantifier) {
    sxml_status status = SXML_STATUS_OK;
    if (quantifier.maximum_count == 0 && !quantifier.is_unbounded) {
        /* x{0}: parsed for its errors and group numbers, then dropped. */
        sxml_regex_counts saved_counts = compiler->counts;
        status = sxml_regex_compiler_emit_atom_copy(compiler, extent);
        uint32_t group_count = compiler->counts.group_count;
        compiler->counts = saved_counts;
        compiler->counts.group_count = group_count;
        return status;
    }
    for (uint32_t copy_number = 0; copy_number < quantifier.minimum_count;
         copy_number++) {
        status = sxml_regex_compiler_emit_atom_copy(compiler, extent);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_regex_operation split_operation =
        quantifier.is_reluctant ? SXML_REGEX_OPERATION_SPLIT_TARGET_FIRST
                                : SXML_REGEX_OPERATION_SPLIT_NEXT_FIRST;
    if (quantifier.is_unbounded) {
        uint32_t loop_start = 0;
        status = sxml_regex_compiler_emit(
            compiler,
            (sxml_regex_instruction){.operation = split_operation,
                                     .operand = 0},
            &loop_start);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint32_t progress_slot = 2 * (compiler->total_group_count + 1) +
                                 compiler->counts.register_count;
        compiler->counts.register_count += 1;
        status = sxml_regex_compiler_emit_operation(
            compiler, SXML_REGEX_OPERATION_SAVE, progress_slot);
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_compiler_emit_atom_copy(compiler, extent);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_compiler_emit_operation(
                compiler, SXML_REGEX_OPERATION_CHECK_PROGRESS, progress_slot);
        }
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
    sxml_regex_pending_targets exits = {.last_instruction_number = 0};
    for (uint32_t copy_number = quantifier.minimum_count;
         copy_number < quantifier.maximum_count; copy_number++) {
        uint32_t split_index = 0;
        status = sxml_regex_compiler_emit(
            compiler,
            (sxml_regex_instruction){.operation = split_operation,
                                     .operand = 0},
            &split_index);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_regex_pending_targets_add(compiler, &exits, split_index);
        status = sxml_regex_compiler_emit_atom_copy(compiler, extent);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_regex_pending_targets_resolve(compiler, exits,
                                       compiler->counts.instruction_count);
    return SXML_STATUS_OK;
}

/* Reads one piece: an assertion, or an atom with an optional quantifier. */
static sxml_status sxml_regex_compiler_parse_piece(
    sxml_regex_compiler *compiler) {
    uint32_t codepoint = 0;
    (void)sxml_regex_compiler_peek(compiler, &codepoint);
    if (codepoint == '^' || codepoint == '$') {
        (void)sxml_regex_compiler_next(compiler, &codepoint);
        sxml_regex_operation operation =
            codepoint == '^'
                ? (compiler->flags.is_multi_line
                       ? SXML_REGEX_OPERATION_LINE_START
                       : SXML_REGEX_OPERATION_STRING_START)
                : (compiler->flags.is_multi_line
                       ? SXML_REGEX_OPERATION_LINE_END
                       : SXML_REGEX_OPERATION_STRING_END);
        uint32_t following_codepoint = 0;
        if (sxml_regex_compiler_peek(compiler, &following_codepoint) &&
            (following_codepoint == '?' || following_codepoint == '*' ||
             following_codepoint == '+' || following_codepoint == '{')) {
            /* F&O 4.0, section 6.1.8. */
            return SXML_REGEX_INVALID(
                compiler, "an assertion cannot have a quantifier");
        }
        return sxml_regex_compiler_emit_operation(compiler, operation, 0);
    }
    sxml_regex_atom_extent extent = {
        .start_byte_offset = compiler->byte_offset,
        .end_byte_offset = 0,
        .group_count_before = compiler->counts.group_count,
    };
    sxml_regex_compiler_skip_atom(compiler);
    extent.end_byte_offset = compiler->byte_offset;
    sxml_regex_quantifier quantifier = {.is_present = false};
    sxml_status status =
        sxml_regex_compiler_parse_quantifier(compiler, &quantifier);
    if (status != SXML_STATUS_OK) {
        /* An error in the atom comes before an error in its quantifier. */
        size_t quantifier_byte_offset = compiler->byte_offset;
        compiler->byte_offset = extent.start_byte_offset;
        sxml_regex_counts saved_counts = compiler->counts;
        sxml_status atom_status = sxml_regex_compiler_parse_atom(compiler);
        compiler->counts = saved_counts;
        compiler->byte_offset = quantifier_byte_offset;
        return atom_status != SXML_STATUS_OK ? atom_status : status;
    }
    size_t after_quantifier_byte_offset = compiler->byte_offset;
    if (quantifier.is_present) {
        status = sxml_regex_compiler_emit_quantified_atom(compiler, extent,
                                                          quantifier);
    } else {
        status = sxml_regex_compiler_emit_atom_copy(compiler, extent);
    }
    compiler->byte_offset = after_quantifier_byte_offset;
    return status;
}

/* Reads a branch: pieces up to "|", ")" or the end. */
static sxml_status sxml_regex_compiler_parse_branch(
    sxml_regex_compiler *compiler) {
    uint32_t codepoint = 0;
    while (sxml_regex_compiler_peek(compiler, &codepoint) && codepoint != '|' &&
           codepoint != ')') {
        sxml_status status = sxml_regex_compiler_parse_piece(compiler);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/*
 * Reads a regExp: branches separated by "|", up to ")" or the end. The
 * first matching branch is chosen (F&O 4.0, section 6.1.1), so each branch
 * but the last starts with a split that prefers it.
 */
static sxml_status sxml_regex_compiler_parse_alternation(
    sxml_regex_compiler *compiler) {
    uint32_t branch_count = sxml_regex_compiler_count_branches(compiler);
    sxml_regex_pending_targets ends = {.last_instruction_number = 0};
    for (uint32_t branch_number = 1; branch_number <= branch_count;
         branch_number++) {
        bool is_last_branch = branch_number == branch_count;
        uint32_t split_index = 0;
        sxml_status status = SXML_STATUS_OK;
        if (!is_last_branch) {
            status = sxml_regex_compiler_emit(
                compiler,
                (sxml_regex_instruction){
                    .operation = SXML_REGEX_OPERATION_SPLIT_NEXT_FIRST,
                    .operand = 0},
                &split_index);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        status = sxml_regex_compiler_parse_branch(compiler);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (is_last_branch) {
            break;
        }
        uint32_t jump_index = 0;
        status = sxml_regex_compiler_emit(
            compiler,
            (sxml_regex_instruction){.operation = SXML_REGEX_OPERATION_JUMP,
                                     .operand = 0},
            &jump_index);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_regex_pending_targets_add(compiler, &ends, jump_index);
        sxml_regex_compiler_patch(compiler, split_index,
                                  compiler->counts.instruction_count);
        uint32_t codepoint = 0;
        if (!sxml_regex_compiler_next(compiler, &codepoint) || codepoint != '|') {
            return SXML_REGEX_INVALID(compiler, "malformed alternation");
        }
    }
    sxml_regex_pending_targets_resolve(compiler, ends,
                                       compiler->counts.instruction_count);
    return SXML_STATUS_OK;
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

/*
 * Runs one pass of the compiler: counting when regex is NULL, otherwise
 * writing into regex, whose arrays are sized by a counting pass and whose
 * group_count that pass found.
 */
static sxml_status sxml_regex_compile(
    const sxml_regex_configuration *configuration, sxml_regex *regex,
    sxml_regex_counts *counts) {
    sxml_regex_compiler compiler = {
        .regex_text = configuration->regex_text,
        .byte_offset = 0,
        .is_in_class = false,
        .flags = configuration->flags,
        .regex = regex,
        .counts = {.instruction_count = 0},
        .total_group_count = regex == NULL ? 0 : regex->group_count,
        .maximum_instruction_count =
            configuration->maximum_instruction_count == 0
                ? SXML_REGEX_DEFAULT_MAXIMUM_INSTRUCTION_COUNT
                : configuration->maximum_instruction_count,
        .maximum_group_depth = configuration->maximum_group_depth == 0
                                   ? SXML_REGEX_DEFAULT_MAXIMUM_GROUP_DEPTH
                                   : configuration->maximum_group_depth,
        .group_depth = 0,
        .atom_parse_count = 0,
        .innermost_open_group = NULL,
        .error_record = configuration->error_record,
    };
    sxml_status status = SXML_STATUS_OK;
    if (compiler.flags.is_literal) {
        status = sxml_regex_compiler_parse_literal_text(&compiler);
    } else {
        status = sxml_regex_compiler_parse_alternation(&compiler);
        uint32_t codepoint = 0;
        if (status == SXML_STATUS_OK &&
            sxml_regex_compiler_peek(&compiler, &codepoint)) {
            status = SXML_REGEX_INVALID(&compiler, "\")\" without \"(\"");
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_compiler_emit_operation(
            &compiler, SXML_REGEX_OPERATION_MATCH, 0);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *counts = compiler.counts;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Regex lifecycle */

/* Where the parts of a compiled regex go in its one block. */
typedef struct sxml_regex_layout {
    sxml_memory_requirements requirements;
    size_t instructions_offset;
    size_t classes_offset;
    size_t class_items_offset;
} sxml_regex_layout;

/* Lays out the header and the three arrays for counts. */
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
        .flags = configuration->flags,
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
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
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
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_regex_memory_requirements(configuration, &requirements);
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
        configuration, &new_regex);
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

/* ------------------------------------------------------------- Matching */

typedef enum sxml_regex_backtrack_kind {
    /* Resume at instruction index and position byte_offset. */
    SXML_REGEX_BACKTRACK_KIND_RESUME,
    /* Put byte_offset back into slot index. */
    SXML_REGEX_BACKTRACK_KIND_RESTORE_SLOT,
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

static bool sxml_regex_class_item_contains(const sxml_regex_class_item *item,
                                           uint32_t codepoint) {
    bool is_member = false;
    switch (item->kind) {
    case SXML_REGEX_CLASS_ITEM_KIND_RANGE:
        is_member = codepoint >= item->first_codepoint &&
                    codepoint <= item->last_codepoint;
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
    }
    return is_member != item->is_complemented;
}

/* True when codepoint is in class class_index (recursion depth bounded by
   maximum_group_depth, through nested subtraction). */
static bool sxml_regex_class_contains(const sxml_regex *regex,
                                      uint32_t class_index,
                                      uint32_t codepoint) {
    const sxml_regex_class *regex_class = &regex->classes[class_index];
    bool is_member = false;
    for (uint32_t item_index = regex_class->first_item_index;
         item_index < regex_class->first_item_index + regex_class->item_count;
         item_index++) {
        if (sxml_regex_class_item_contains(&regex->class_items[item_index],
                                           codepoint)) {
            is_member = true;
            break;
        }
    }
    if (regex_class->is_negated) {
        is_member = !is_member;
    }
    if (is_member && regex_class->subtracted_class_number != 0) {
        is_member = !sxml_regex_class_contains(
            regex, regex_class->subtracted_class_number - 1, codepoint);
    }
    return is_member;
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

/* The state of one anchored attempt. */
typedef struct sxml_regex_thread {
    uint32_t instruction_index;
    size_t byte_offset;
    /* Set by a step when the thread cannot go on and must backtrack. */
    bool has_failed;
} sxml_regex_thread;

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
        status = sxml_regex_matcher_push(
            matcher, (sxml_regex_backtrack_entry){
                         .kind = SXML_REGEX_BACKTRACK_KIND_RESTORE_SLOT,
                         .index = instruction.operand,
                         .byte_offset = matcher->slots[instruction.operand]});
        matcher->slots[instruction.operand] = thread->byte_offset;
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
        case SXML_REGEX_BACKTRACK_KIND_RESUME:
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
