/*
 * The stemming machine of Full Text (src/full_text_internal.h,
 * "Stemming"; Full Text 3.0, 3.4.4; packet 7.12c, D214; glossary
 * `snowball_program`). Part of the optional feature full-text.
 *
 * It runs a compiled Snowball program (src/full_text_stem_table.c) over a
 * word's code points, as the Snowball manual defines the language
 * (snowballstem.org, "Snowball Manual") and its runtimes implement it:
 * a cursor between characters, a limit, a backward limit, a slice
 * [slice_start, slice_end) and a signal, true or false, that every
 * command sets. In backward mode the cursor moves left, toward the
 * backward limit, and a saved cursor is kept as its distance from the
 * limit, so changes to the right of it do not move it. One loop over the
 * instructions; routine calls push their return addresses on an explicit
 * stack (D71); no routine is recursive (the generator checks).
 *
 * Golden file: src/regex.c (a compiled program run by one loop, D53).
 */
#include "full_text_internal.h"

#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* The most instructions one word may take; Snowball's algorithms take a
   few thousand for the longest words, so reaching it means a program
   that loops (an error, never a silent result). */
#define SXML_FULL_TEXT_STEM_STEP_BOUND 4000000U

/* The longest a word may grow to while it is stemmed. */
#define SXML_FULL_TEXT_STEM_MAXIMUM_TEXT 65536U

/* ------------------------------------------------------------ Languages */

sxml_full_text_language sxml_full_text_language_of(sxml_byte_view language_tag) {
    static const char *const subtags[SXML_FULL_TEXT_LANGUAGE_COUNT] = {"en", "de", "fr",
                                                                       "it", "es", "nl"};
    size_t subtag_length = 0;
    while (subtag_length < language_tag.byte_count && language_tag.bytes[subtag_length] != '-') {
        subtag_length += 1;
    }
    for (size_t language = 0; language < SXML_FULL_TEXT_LANGUAGE_COUNT; language += 1) {
        bool is_same = subtag_length == 2;
        for (size_t byte_index = 0; is_same && byte_index < 2; byte_index += 1) {
            uint8_t byte = language_tag.bytes[byte_index];
            if (byte >= 'A' && byte <= 'Z') {
                byte = (uint8_t)(byte + ('a' - 'A'));
            }
            is_same = byte == (uint8_t)subtags[language][byte_index];
        }
        if (is_same) {
            return (sxml_full_text_language)language;
        }
    }
    return SXML_FULL_TEXT_LANGUAGE_ENGLISH;
}

/* ------------------------------------------------------------- Workspace */

void sxml_full_text_stem_workspace_release(sxml_full_text_stem_workspace *workspace) {
    sxml_allocator_release(workspace->allocator, workspace->text);
    sxml_allocator_release(workspace->allocator, workspace->variables);
    sxml_allocator_release(workspace->allocator, workspace->registers);
    workspace->text = NULL;
    workspace->text_capacity = 0;
    workspace->variables = NULL;
    workspace->variable_capacity = 0;
    workspace->registers = NULL;
    workspace->register_capacity = 0;
}

/* Grows *elements (of element_size bytes each, *capacity of them) to hold
   required, keeping kept_count of them. */
typedef struct workspace_growth {
    void **elements;
    size_t *capacity;
    size_t required;
    size_t kept_count;
    size_t element_size;
} workspace_growth;

static sxml_status grow(sxml_allocator allocator, workspace_growth growth) {
    if (growth.required <= *growth.capacity) {
        return SXML_STATUS_OK;
    }
    size_t capacity = *growth.capacity == 0 ? 32U : *growth.capacity;
    while (capacity < growth.required) {
        capacity *= 2U;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = capacity * growth.element_size,
                                  .alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (growth.kept_count != 0) {
        memcpy(memory, *growth.elements, growth.kept_count * growth.element_size);
    }
    sxml_allocator_release(allocator, *growth.elements);
    *growth.elements = memory;
    *growth.capacity = capacity;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Machine */

typedef struct stem_machine {
    const sxml_full_text_stemmer_program *program;
    sxml_full_text_stem_workspace *workspace;
    int32_t length;
    int32_t cursor;
    int32_t limit;
    int32_t backward_limit;
    int32_t slice_start;
    int32_t slice_end;
    bool is_backward;
    bool signal;
    int32_t *slots;
    int32_t *limit_slots;
    int32_t *integers;
    int32_t *booleans;
    int32_t *among_results;
    int32_t *variable_lengths;
    int32_t *return_stack;
    size_t return_depth;
    size_t variable_stride;
} stem_machine;

/* A replacement's characters: count of them, from the program's pool
   (narrow) or from a string variable (wide). */
typedef struct replacement {
    const uint16_t *narrow;
    const uint32_t *wide;
    int32_t count;
} replacement;

static uint32_t replacement_at(replacement characters, int32_t index) {
    if (characters.narrow != NULL) {
        return characters.narrow[index];
    }
    return characters.wide != NULL ? characters.wide[index] : 0;
}

/* The program's string number string_number. */
static replacement program_string(const stem_machine *machine, uint16_t string_number) {
    const sxml_full_text_stem_span span = machine->program->strings[string_number];
    return (replacement){.narrow = machine->program->characters + span.first,
                         .wide = NULL,
                         .count = (int32_t)span.count};
}

/* The span [start, end) of the text, and what it becomes. */
typedef struct text_change {
    int32_t start;
    int32_t end;
    replacement characters;
} text_change;

/* Snowball's replace_s: the text between start and end becomes the
   characters; the limit follows, and the cursor moves with the text after
   the change (or to its start when it was inside). Writes the change of
   length to *adjustment. */
static sxml_status replace_text(stem_machine *machine, text_change change, int32_t *adjustment) {
    sxml_full_text_stem_workspace *workspace = machine->workspace;
    const int32_t difference = change.characters.count - (change.end - change.start);
    const int32_t new_length = machine->length + difference;
    if (new_length < 0 || (uint32_t)new_length > SXML_FULL_TEXT_STEM_MAXIMUM_TEXT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status = grow(workspace->allocator,
                              (workspace_growth){.elements = (void **)&workspace->text,
                                                 .capacity = &workspace->text_capacity,
                                                 .required = (size_t)new_length + 1U,
                                                 .kept_count = (size_t)machine->length,
                                                 .element_size = sizeof(uint32_t)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *text = workspace->text;
    memmove(text + change.start + change.characters.count, text + change.end,
            (size_t)(machine->length - change.end) * sizeof(uint32_t));
    for (int32_t index = 0; index < change.characters.count; index += 1) {
        text[change.start + index] = replacement_at(change.characters, index);
    }
    machine->length = new_length;
    machine->limit += difference;
    if (machine->cursor >= change.end) {
        machine->cursor += difference;
    } else if (machine->cursor > change.start) {
        machine->cursor = change.start;
    }
    *adjustment = difference;
    return SXML_STATUS_OK;
}

/* Whether the characters are at the cursor (before it, backward), and
   then moves over them. */
static bool match_characters(stem_machine *machine, replacement characters) {
    const uint32_t *text = machine->workspace->text;
    const int32_t count = characters.count;
    if (machine->is_backward) {
        if (machine->cursor - machine->backward_limit < count) {
            return false;
        }
        const int32_t start = machine->cursor - count;
        for (int32_t index = 0; index < count; index += 1) {
            if (text[start + index] != replacement_at(characters, index)) {
                return false;
            }
        }
        machine->cursor = start;
        return true;
    }
    if (machine->limit - machine->cursor < count) {
        return false;
    }
    for (int32_t index = 0; index < count; index += 1) {
        if (text[machine->cursor + index] != replacement_at(characters, index)) {
            return false;
        }
    }
    machine->cursor += count;
    return true;
}

/* Whether codepoint is in grouping grouping_number (sorted code points). */
typedef struct grouping_query {
    uint16_t grouping_number;
    uint32_t codepoint;
} grouping_query;

static bool is_in_grouping(const stem_machine *machine, grouping_query query) {
    const sxml_full_text_stem_span span = machine->program->groupings[query.grouping_number];
    const uint16_t *members = machine->program->characters + span.first;
    size_t low = 0;
    size_t high = span.count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (members[middle] == query.codepoint) {
            return true;
        }
        if (members[middle] < query.codepoint) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return false;
}

/* in_grouping and out_grouping: the next character (the previous one,
   backward) is (is not) in the grouping, and the cursor moves over it. */
typedef struct grouping_test {
    uint16_t grouping_number;
    bool wants_member;
} grouping_test;

static bool test_grouping(stem_machine *machine, grouping_test test) {
    const uint32_t *text = machine->workspace->text;
    if (machine->is_backward) {
        if (machine->cursor <= machine->backward_limit) {
            return false;
        }
        const grouping_query query = {.grouping_number = test.grouping_number,
                                      .codepoint = text[machine->cursor - 1]};
        if (is_in_grouping(machine, query) != test.wants_member) {
            return false;
        }
        machine->cursor -= 1;
        return true;
    }
    if (machine->cursor >= machine->limit) {
        return false;
    }
    const grouping_query query = {.grouping_number = test.grouping_number,
                                  .codepoint = text[machine->cursor]};
    if (is_in_grouping(machine, query) != test.wants_member) {
        return false;
    }
    machine->cursor += 1;
    return true;
}

/* hop count (next is hop 1). */
static bool hop(stem_machine *machine, int32_t count) {
    if (machine->is_backward) {
        if (machine->cursor - count < machine->backward_limit) {
            return false;
        }
        machine->cursor -= count;
        return true;
    }
    if (machine->cursor + count > machine->limit) {
        return false;
    }
    machine->cursor += count;
    return true;
}

/* An among: the first (longest) of its strings at the cursor; its result
   (0 for none) goes to among_results. */
static bool find_among(stem_machine *machine, uint16_t among_number) {
    const sxml_full_text_stem_span among = machine->program->amongs[among_number];
    machine->among_results[among_number] = 0;
    for (uint16_t entry = 0; entry < among.count; entry += 1) {
        const sxml_full_text_stem_among_entry *candidate =
            &machine->program->among_entries[among.first + entry];
        if (match_characters(machine, program_string(machine, candidate->string))) {
            machine->among_results[among_number] = candidate->result;
            return true;
        }
    }
    return false;
}

/* An integer expression: kind 0 constant, 1 cursor, 2 limit, 3 variable. */
typedef struct integer_expression {
    uint16_t kind;
    uint16_t value;
} integer_expression;

static int32_t evaluate(const stem_machine *machine, integer_expression expression) {
    switch (expression.kind) {
    case 0:
        return (int32_t)expression.value;
    case 1:
        return machine->cursor;
    case 2:
        return machine->is_backward ? machine->backward_limit : machine->limit;
    default:
        return machine->integers[expression.value];
    }
}

static bool compare(int32_t left, uint16_t relation, int32_t right) {
    switch (relation) {
    case 0:
        return left == right;
    case 1:
        return left != right;
    case 2:
        return left < right;
    case 3:
        return left <= right;
    case 4:
        return left > right;
    default:
        return left >= right;
    }
}

/* insert and attach: the characters go in at the cursor, the slice moves
   with the text after it; the cursor ends after them (insert forward,
   attach backward) or stays (the other two). */
typedef struct insertion {
    replacement characters;
    bool moves_cursor;
} insertion;

static sxml_status insert_text(stem_machine *machine, insertion request) {
    const int32_t saved = machine->cursor;
    int32_t adjustment = 0;
    const sxml_status status = replace_text(
        machine, (text_change){.start = saved, .end = saved, .characters = request.characters},
        &adjustment);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (saved <= machine->slice_start) {
        machine->slice_start += adjustment;
    }
    if (saved <= machine->slice_end) {
        machine->slice_end += adjustment;
    }
    if (!request.moves_cursor) {
        machine->cursor = saved;
    }
    return SXML_STATUS_OK;
}

/* A string variable's characters. */
static replacement variable_string(const stem_machine *machine, uint16_t variable) {
    return (replacement){.narrow = NULL,
                         .wide = machine->workspace->variables + variable * machine->variable_stride,
                         .count = machine->variable_lengths[variable]};
}

/* ->variable: the slice becomes the variable's value. */
static sxml_status slice_to(stem_machine *machine, uint16_t variable) {
    sxml_full_text_stem_workspace *workspace = machine->workspace;
    const size_t slice_length = (size_t)(machine->slice_end - machine->slice_start);
    const size_t variable_count = machine->program->string_variable_count;
    if (slice_length > machine->variable_stride) {
        /* A wider stride: move each variable to its new place. */
        size_t stride = machine->variable_stride == 0 ? 16U : machine->variable_stride;
        while (stride < slice_length) {
            stride *= 2U;
        }
        uint32_t *memory = NULL;
        sxml_status status = sxml_allocator_allocate(
            workspace->allocator,
            (sxml_allocation_request){.byte_count = stride * variable_count * sizeof(uint32_t),
                                      .alignment = alignof(uint32_t)},
            (void **)&memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        for (size_t number = 0; number < variable_count && machine->variable_stride != 0;
             number += 1) {
            memcpy(memory + number * stride, workspace->variables + number * machine->variable_stride,
                   (size_t)machine->variable_lengths[number] * sizeof(uint32_t));
        }
        sxml_allocator_release(workspace->allocator, workspace->variables);
        workspace->variables = memory;
        workspace->variable_capacity = stride * variable_count;
        machine->variable_stride = stride;
    }
    memcpy(workspace->variables + variable * machine->variable_stride,
           workspace->text + machine->slice_start, slice_length * sizeof(uint32_t));
    machine->variable_lengths[variable] = (int32_t)slice_length;
    return SXML_STATUS_OK;
}

/* The operands an instruction has (after its opcode). */
static size_t operand_count(uint16_t opcode) {
    switch ((uint32_t)opcode) {
    case SXML_FULL_TEXT_STEM_TRUE:
    case SXML_FULL_TEXT_STEM_FALSE:
    case SXML_FULL_TEXT_STEM_BACKWARD_BEGIN:
    case SXML_FULL_TEXT_STEM_BACKWARD_END:
    case SXML_FULL_TEXT_STEM_NEXT:
    case SXML_FULL_TEXT_STEM_AT_LIMIT:
    case SXML_FULL_TEXT_STEM_OPEN_SLICE:
    case SXML_FULL_TEXT_STEM_CLOSE_SLICE:
    case SXML_FULL_TEXT_STEM_RETURN:
        return 0;
    case SXML_FULL_TEXT_STEM_INTEGER_ASSIGN:
        return 3;
    case SXML_FULL_TEXT_STEM_INTEGER_COMPARE:
        return 4;
    case SXML_FULL_TEXT_STEM_BOOLEAN_SET:
    case SXML_FULL_TEXT_STEM_AMONG_DISPATCH:
        return 2;
    default:
        return 1;
    }
}

/* Runs one instruction whose operands are operands; *next is the address
   of the next one, which jumps, calls and returns change. */
typedef struct step {
    uint16_t opcode;
    /* The first operand, 0 when there is none. */
    uint16_t operand;
    const uint16_t *operands;
    size_t *next;
} step;

static sxml_status control(stem_machine *machine, step current, bool *is_done) {
    const uint16_t operand = current.operand;
    switch ((uint32_t)current.opcode) {
    case SXML_FULL_TEXT_STEM_JUMP:
        *current.next = operand;
        break;
    case SXML_FULL_TEXT_STEM_JUMP_IF_TRUE:
        if (machine->signal) {
            *current.next = operand;
        }
        break;
    case SXML_FULL_TEXT_STEM_JUMP_IF_FALSE:
        if (!machine->signal) {
            *current.next = operand;
        }
        break;
    case SXML_FULL_TEXT_STEM_CALL:
        if (machine->return_depth == machine->program->call_depth) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        machine->return_stack[machine->return_depth] = (int32_t)*current.next;
        machine->return_depth += 1;
        *current.next = operand;
        break;
    case SXML_FULL_TEXT_STEM_RETURN:
        if (machine->return_depth == 0) {
            *is_done = true;
            break;
        }
        machine->return_depth -= 1;
        *current.next = (size_t)machine->return_stack[machine->return_depth];
        break;
    case SXML_FULL_TEXT_STEM_AMONG_DISPATCH: {
        /* The action of the among's result: the result-th address after
           the instruction. */
        const int32_t result = machine->among_results[operand];
        if (result < 1 || (uint32_t)result > current.operands[1]) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        *current.next = current.operands[1 + result];
        break;
    }
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

/* Saving and restoring the cursor and the limits. */
static void save_and_restore(stem_machine *machine, step current) {
    const uint16_t operand = current.operand;
    switch ((uint32_t)current.opcode) {
    case SXML_FULL_TEXT_STEM_SAVE:
        machine->slots[operand] =
            machine->is_backward ? machine->limit - machine->cursor : machine->cursor;
        break;
    case SXML_FULL_TEXT_STEM_RESTORE:
        machine->cursor =
            machine->is_backward ? machine->limit - machine->slots[operand] : machine->slots[operand];
        break;
    case SXML_FULL_TEXT_STEM_LIMIT_SAVE:
        /* setlimit: the cursor after its first command becomes the limit
           (forward, kept as the distance to the old one) or the backward
           limit. */
        if (machine->is_backward) {
            machine->limit_slots[operand] = machine->backward_limit;
            machine->backward_limit = machine->cursor;
        } else {
            machine->limit_slots[operand] = machine->limit - machine->cursor;
            machine->limit = machine->cursor;
        }
        machine->signal = true;
        break;
    case SXML_FULL_TEXT_STEM_LIMIT_RESTORE:
        if (machine->is_backward) {
            machine->backward_limit = machine->limit_slots[operand];
        } else {
            machine->limit += machine->limit_slots[operand];
        }
        break;
    case SXML_FULL_TEXT_STEM_BACKWARD_BEGIN:
        machine->backward_limit = machine->cursor;
        machine->cursor = machine->limit;
        machine->is_backward = true;
        machine->signal = true;
        break;
    default:
        machine->cursor = machine->backward_limit;
        machine->is_backward = false;
        break;
    }
}

/* Tests of the text, the marks, integers and booleans. */
static void test(stem_machine *machine, step current) {
    const uint16_t operand = current.operand;
    switch ((uint32_t)current.opcode) {
    case SXML_FULL_TEXT_STEM_TRUE:
        machine->signal = true;
        break;
    case SXML_FULL_TEXT_STEM_FALSE:
        machine->signal = false;
        break;
    case SXML_FULL_TEXT_STEM_LITERAL:
        machine->signal = match_characters(machine, program_string(machine, operand));
        break;
    case SXML_FULL_TEXT_STEM_IN_GROUPING:
    case SXML_FULL_TEXT_STEM_OUT_GROUPING:
        machine->signal = test_grouping(
            machine, (grouping_test){.grouping_number = operand,
                                     .wants_member = current.opcode ==
                                                     SXML_FULL_TEXT_STEM_IN_GROUPING});
        break;
    case SXML_FULL_TEXT_STEM_NEXT:
        machine->signal = hop(machine, 1);
        break;
    case SXML_FULL_TEXT_STEM_HOP:
        machine->signal = hop(machine, (int32_t)operand);
        break;
    case SXML_FULL_TEXT_STEM_AT_LIMIT:
        machine->signal = machine->cursor ==
                          (machine->is_backward ? machine->backward_limit : machine->limit);
        break;
    case SXML_FULL_TEXT_STEM_SET_MARK:
        machine->integers[operand] = machine->cursor;
        machine->signal = true;
        break;
    case SXML_FULL_TEXT_STEM_TO_MARK: {
        const int32_t mark = machine->integers[operand];
        machine->signal = machine->is_backward ? machine->cursor >= mark : machine->cursor <= mark;
        if (machine->signal) {
            machine->cursor = mark;
        }
        break;
    }
    case SXML_FULL_TEXT_STEM_AT_MARK:
        machine->signal = machine->cursor == machine->integers[operand];
        break;
    case SXML_FULL_TEXT_STEM_INTEGER_ASSIGN:
        machine->integers[operand] = evaluate(
            machine,
            (integer_expression){.kind = current.operands[1], .value = current.operands[2]});
        machine->signal = true;
        break;
    case SXML_FULL_TEXT_STEM_INTEGER_COMPARE:
        machine->signal = compare(
            machine->integers[operand], current.operands[1],
            evaluate(machine, (integer_expression){.kind = current.operands[2],
                                                   .value = current.operands[3]}));
        break;
    case SXML_FULL_TEXT_STEM_BOOLEAN_SET:
        machine->booleans[operand] = current.operands[1];
        machine->signal = true;
        break;
    case SXML_FULL_TEXT_STEM_BOOLEAN_TEST:
        machine->signal = machine->booleans[operand] != 0;
        break;
    default:
        /* AMONG_FIND. */
        machine->signal = find_among(machine, operand);
        break;
    }
}

/* The slice and the changes of the text. */
static sxml_status change(stem_machine *machine, step current) {
    const uint16_t operand = current.operand;
    sxml_status status = SXML_STATUS_OK;
    switch ((uint32_t)current.opcode) {
    case SXML_FULL_TEXT_STEM_OPEN_SLICE:
        if (machine->is_backward) {
            machine->slice_end = machine->cursor;
        } else {
            machine->slice_start = machine->cursor;
        }
        break;
    case SXML_FULL_TEXT_STEM_CLOSE_SLICE:
        if (machine->is_backward) {
            machine->slice_start = machine->cursor;
        } else {
            machine->slice_end = machine->cursor;
        }
        break;
    case SXML_FULL_TEXT_STEM_REPLACE: {
        /* <- and delete (Snowball's slice_from): the slice becomes the
           string. */
        if (machine->slice_start < 0 || machine->slice_start > machine->slice_end ||
            machine->slice_end > machine->limit) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        const replacement characters = program_string(machine, operand);
        int32_t adjustment = 0;
        status = replace_text(machine,
                              (text_change){.start = machine->slice_start,
                                            .end = machine->slice_end,
                                            .characters = characters},
                              &adjustment);
        machine->slice_end = machine->slice_start + characters.count;
        break;
    }
    case SXML_FULL_TEXT_STEM_SLICE_TO:
        status = slice_to(machine, operand);
        break;
    case SXML_FULL_TEXT_STEM_INSERT:
    case SXML_FULL_TEXT_STEM_ATTACH:
        status = insert_text(
            machine, (insertion){.characters = program_string(machine, operand),
                                 .moves_cursor = (current.opcode == SXML_FULL_TEXT_STEM_INSERT) !=
                                                 machine->is_backward});
        break;
    default:
        status = insert_text(
            machine,
            (insertion){.characters = variable_string(machine, operand),
                        .moves_cursor = (current.opcode == SXML_FULL_TEXT_STEM_INSERT_VARIABLE) !=
                                        machine->is_backward});
        break;
    }
    machine->signal = true;
    return status;
}

/* Whether the operands of the instruction at address name what the
   program has (the generator's programs always do). */
static bool is_valid(const stem_machine *machine, size_t address) {
    const sxml_full_text_stemmer_program *program = machine->program;
    const uint16_t opcode = program->code[address];
    if (opcode > (uint16_t)SXML_FULL_TEXT_STEM_AMONG_DISPATCH ||
        address + operand_count(opcode) >= program->code_count) {
        return false;
    }
    const uint16_t operand = operand_count(opcode) != 0 ? program->code[address + 1] : 0;
    switch ((uint32_t)opcode) {
    case SXML_FULL_TEXT_STEM_SAVE:
    case SXML_FULL_TEXT_STEM_RESTORE:
        return operand < program->slot_count;
    case SXML_FULL_TEXT_STEM_LIMIT_SAVE:
    case SXML_FULL_TEXT_STEM_LIMIT_RESTORE:
        return operand < program->limit_slot_count;
    case SXML_FULL_TEXT_STEM_SET_MARK:
    case SXML_FULL_TEXT_STEM_TO_MARK:
    case SXML_FULL_TEXT_STEM_AT_MARK:
    case SXML_FULL_TEXT_STEM_INTEGER_ASSIGN:
    case SXML_FULL_TEXT_STEM_INTEGER_COMPARE:
        return operand < program->integer_count;
    case SXML_FULL_TEXT_STEM_BOOLEAN_SET:
    case SXML_FULL_TEXT_STEM_BOOLEAN_TEST:
        return operand < program->boolean_count;
    case SXML_FULL_TEXT_STEM_SLICE_TO:
    case SXML_FULL_TEXT_STEM_INSERT_VARIABLE:
    case SXML_FULL_TEXT_STEM_ATTACH_VARIABLE:
        return operand < program->string_variable_count;
    case SXML_FULL_TEXT_STEM_AMONG_FIND:
        return operand < program->among_count;
    case SXML_FULL_TEXT_STEM_AMONG_DISPATCH:
        return operand < program->among_count &&
               address + 2U + program->code[address + 2] < program->code_count;
    default:
        return true;
    }
}

static sxml_status run(stem_machine *machine) {
    const sxml_full_text_stemmer_program *program = machine->program;
    size_t address = 0;
    bool is_done = false;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t step_count = 0; status == SXML_STATUS_OK && !is_done; step_count += 1) {
        if (step_count == SXML_FULL_TEXT_STEM_STEP_BOUND) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        if (address >= program->code_count || !is_valid(machine, address)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        const uint16_t opcode = program->code[address];
        size_t next = address + 1U + operand_count(opcode);
        const step current = {.opcode = opcode,
                              .operand = operand_count(opcode) != 0 ? program->code[address + 1] : 0,
                              .operands = program->code + address + 1,
                              .next = &next};
        switch ((uint32_t)opcode) {
        case SXML_FULL_TEXT_STEM_JUMP:
        case SXML_FULL_TEXT_STEM_JUMP_IF_TRUE:
        case SXML_FULL_TEXT_STEM_JUMP_IF_FALSE:
        case SXML_FULL_TEXT_STEM_CALL:
        case SXML_FULL_TEXT_STEM_RETURN:
        case SXML_FULL_TEXT_STEM_AMONG_DISPATCH:
            status = control(machine, current, &is_done);
            break;
        case SXML_FULL_TEXT_STEM_SAVE:
        case SXML_FULL_TEXT_STEM_RESTORE:
        case SXML_FULL_TEXT_STEM_LIMIT_SAVE:
        case SXML_FULL_TEXT_STEM_LIMIT_RESTORE:
        case SXML_FULL_TEXT_STEM_BACKWARD_BEGIN:
        case SXML_FULL_TEXT_STEM_BACKWARD_END:
            save_and_restore(machine, current);
            break;
        case SXML_FULL_TEXT_STEM_OPEN_SLICE:
        case SXML_FULL_TEXT_STEM_CLOSE_SLICE:
        case SXML_FULL_TEXT_STEM_REPLACE:
        case SXML_FULL_TEXT_STEM_SLICE_TO:
        case SXML_FULL_TEXT_STEM_INSERT:
        case SXML_FULL_TEXT_STEM_INSERT_VARIABLE:
        case SXML_FULL_TEXT_STEM_ATTACH:
        case SXML_FULL_TEXT_STEM_ATTACH_VARIABLE:
            status = change(machine, current);
            break;
        default:
            test(machine, current);
            break;
        }
        address = next;
    }
    return status;
}

/* Decodes the word into the workspace's text; false when it is longer
   than SXML_FULL_TEXT_MAXIMUM_STEMMED_LENGTH code points. */
typedef struct decoding {
    bool is_stemmable;
} decoding;

static sxml_status decode_word(stem_machine *machine, sxml_byte_view word, decoding *outcome) {
    sxml_full_text_stem_workspace *workspace = machine->workspace;
    outcome->is_stemmable = false;
    if (word.byte_count > (size_t)SXML_FULL_TEXT_MAXIMUM_STEMMED_LENGTH * 4U) {
        return SXML_STATUS_OK;
    }
    const sxml_status status =
        grow(workspace->allocator, (workspace_growth){.elements = (void **)&workspace->text,
                                                      .capacity = &workspace->text_capacity,
                                                      .required = word.byte_count * 2U + 16U,
                                                      .kept_count = 0,
                                                      .element_size = sizeof(uint32_t)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    int32_t length = 0;
    for (size_t byte_offset = 0; byte_offset < word.byte_count;) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(word, byte_offset);
        if (decoded.encoded_byte_count == 0 || length == (int32_t)SXML_FULL_TEXT_MAXIMUM_STEMMED_LENGTH) {
            return SXML_STATUS_OK;
        }
        workspace->text[length] = decoded.codepoint;
        length += 1;
        byte_offset += decoded.encoded_byte_count;
    }
    machine->length = length;
    outcome->is_stemmable = true;
    return SXML_STATUS_OK;
}

/* Lays out the registers of the program in the workspace. */
static sxml_status prepare_registers(stem_machine *machine) {
    const sxml_full_text_stemmer_program *program = machine->program;
    sxml_full_text_stem_workspace *workspace = machine->workspace;
    const size_t register_count = (size_t)program->slot_count + program->limit_slot_count +
                                  program->integer_count + program->boolean_count +
                                  program->among_count + program->string_variable_count +
                                  program->call_depth;
    const sxml_status status =
        grow(workspace->allocator, (workspace_growth){.elements = (void **)&workspace->registers,
                                                      .capacity = &workspace->register_capacity,
                                                      .required = register_count,
                                                      .kept_count = 0,
                                                      .element_size = sizeof(int32_t)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(workspace->registers, 0, register_count * sizeof(int32_t));
    int32_t *next = workspace->registers;
    machine->slots = next;
    next += program->slot_count;
    machine->limit_slots = next;
    next += program->limit_slot_count;
    machine->integers = next;
    next += program->integer_count;
    machine->booleans = next;
    next += program->boolean_count;
    machine->among_results = next;
    next += program->among_count;
    machine->variable_lengths = next;
    next += program->string_variable_count;
    machine->return_stack = next;
    machine->variable_stride =
        program->string_variable_count == 0
            ? 0
            : workspace->variable_capacity / program->string_variable_count;
    return SXML_STATUS_OK;
}

sxml_status sxml_full_text_stem(const sxml_full_text_stem_request *request,
                                sxml_xml_buffer *output) {
    if ((uint32_t)request->language >= SXML_FULL_TEXT_LANGUAGE_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    stem_machine machine = {.program = &sxml_full_text_stemmer_programs[request->language],
                            .workspace = request->workspace};
    decoding outcome = {.is_stemmable = false};
    sxml_status status = decode_word(&machine, request->word, &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!outcome.is_stemmable) {
        /* Not stemmed: the word as it is. */
        return sxml_xml_buffer_append(output, request->word);
    }
    status = prepare_registers(&machine);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Snowball's set_current: the cursor at 0, the limit at the end, the
       slice the whole word. */
    machine.limit = machine.length;
    machine.slice_end = machine.length;
    machine.signal = true;
    status = run(&machine);
    for (int32_t index = 0; status == SXML_STATUS_OK && index < machine.length; index += 1) {
        status = sxml_xml_buffer_append_codepoint(output, request->workspace->text[index]);
    }
    return status;
}
