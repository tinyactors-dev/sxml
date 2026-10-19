/*
 * External scanner for the sxml XML 1.0 grammar (grammars/xml/grammar.js).
 *
 * Hand-written; tools/generate-grammars.sh leaves this file alone. It is
 * built with the third-party flags like the generated parser (D12), but
 * follows the naming rules of docs/C_GUIDELINES.md where tree-sitter's
 * fixed entry points allow it.
 *
 * Section numbers refer to Extensible Markup Language (XML) 1.0 (Fifth
 * Edition), https://www.w3.org/TR/xml/.
 *
 * The scanner produces the tokens a regular expression cannot:
 * - the start of a tag with its element name, checked against a stack of
 *   open start tags so that a mismatched end tag is a parse error (WFC:
 *   Element Type Match, 3); a tag without attributes is one token, and so
 *   is a whole end tag, because every token costs the parser far more than
 *   its characters (packet perf-3);
 * - an attribute whose value holds no reference, Name Eq AttValue (perf-3
 *   too);
 * - character data, CDATA text, comment text and processing-instruction
 *   content, which end at a delimiter and must not contain it, and whose
 *   characters must all match [2] Char;
 * - processing-instruction targets, which exclude the name "xml" in any case;
 * - in the external subset, a whole markup declaration that holds a
 *   parameter-entity reference outside its literals (2.8, 4.4.8), which only
 *   the tree builder can expand, and the text of ignored sections (3.4).
 *
 * Tag stack state. tree-sitter serializes the scanner state after every
 * external token into at most TREE_SITTER_SERIALIZATION_BUFFER_SIZE bytes
 * and restores it before every external scan. A stack of names does not fit
 * there for deep documents, so the stack is persistent: every distinct
 * (parent frame, name) pair is stored once, append-only, in the scanner
 * object, and the serialized state is only the index of the innermost frame.
 * Equal stacks get equal indices, which keeps tree-sitter's state comparison
 * exact. Frames live as long as the scanner, that is, as long as the
 * TSParser has this language set.
 *
 * Memory comes from ts_malloc / ts_realloc / ts_free, the tree-sitter
 * runtime's allocator (the D12 exemption for the parse step).
 */

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Must match the order of `externals` in grammar.js. */
enum token_type {
    START_TAG_OPEN,
    START_TAG_WHOLE,
    EMPTY_ELEMENT_TAG_WHOLE,
    END_TAG,
    EMPTY_ELEMENT_TAG_CLOSE,
    ATTRIBUTE_WHOLE,
    CHARACTER_DATA,
    CDATA_TEXT,
    COMMENT_TEXT,
    PROCESSING_INSTRUCTION_TARGET,
    PROCESSING_INSTRUCTION_CONTENT,
    UNEXPANDED_DECLARATION,
    IGNORED_TEXT,
    ERROR_SENTINEL,
};

/* Frame index 0 is the empty stack. */
enum { EMPTY_STACK_FRAME = 0 };

typedef struct {
    uint32_t parent_frame;
    uint32_t name_offset;
    uint32_t name_byte_count;
    uint32_t hash;
} tag_frame;

typedef struct {
    tag_frame *frames;
    uint32_t frame_count;
    uint32_t frame_capacity;

    uint8_t *name_bytes;
    uint32_t name_byte_count;
    uint32_t name_byte_capacity;

    /* Open-addressing hash table of frame indices; 0 marks an empty slot.
     * slot_capacity is zero or a power of two. */
    uint32_t *slots;
    uint32_t slot_capacity;

    /* The name being scanned, UTF-8 encoded. */
    uint8_t *scanned_name;
    uint32_t scanned_name_byte_count;
    uint32_t scanned_name_capacity;

    uint32_t current_frame;
} scanner_state;

/* --- Character classes ------------------------------------------------- */

typedef struct {
    int32_t first;
    int32_t last;
} codepoint_range;

/* [4] NameStartChar */
static const codepoint_range name_start_ranges[] = {
    {':', ':'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'},
    {0xC0, 0xD6}, {0xD8, 0xF6}, {0xF8, 0x2FF}, {0x370, 0x37D},
    {0x37F, 0x1FFF}, {0x200C, 0x200D}, {0x2070, 0x218F}, {0x2C00, 0x2FEF},
    {0x3001, 0xD7FF}, {0xF900, 0xFDCF}, {0xFDF0, 0xFFFD}, {0x10000, 0xEFFFF},
};

/* [4a] NameChar, beyond NameStartChar */
static const codepoint_range name_extra_ranges[] = {
    {'-', '.'}, {'0', '9'}, {0xB7, 0xB7}, {0x300, 0x36F}, {0x203F, 0x2040},
};

static bool is_in_ranges(int32_t codepoint, const codepoint_range *ranges, size_t range_count) {
    for (size_t range_index = 0; range_index < range_count; range_index += 1) {
        if (codepoint >= ranges[range_index].first && codepoint <= ranges[range_index].last) {
            return true;
        }
    }
    return false;
}

/* ASCII first: nearly every name is ASCII. */
static bool is_ascii_name_start_character(int32_t codepoint) {
    return (codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') ||
           codepoint == '_' || codepoint == ':';
}

static bool is_name_start_character(int32_t codepoint) {
    if (codepoint < 0x80) {
        return is_ascii_name_start_character(codepoint);
    }
    return is_in_ranges(codepoint, name_start_ranges,
                        sizeof name_start_ranges / sizeof name_start_ranges[0]);
}

static bool is_name_character(int32_t codepoint) {
    if (codepoint < 0x80) {
        return is_ascii_name_start_character(codepoint) || codepoint == '-' ||
               codepoint == '.' || (codepoint >= '0' && codepoint <= '9');
    }
    return is_name_start_character(codepoint) ||
           is_in_ranges(codepoint, name_extra_ranges,
                        sizeof name_extra_ranges / sizeof name_extra_ranges[0]);
}

/* [2] Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
 * tree-sitter reports undecodable input as a negative lookahead, which this
 * rejects too. */
static bool is_xml_character(int32_t codepoint) {
    return (codepoint >= 0x20 && codepoint <= 0xD7FF) || codepoint == 0x9 || codepoint == 0xA || codepoint == 0xD ||
           (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
           (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
}

/* [3] S */
static bool is_space_character(int32_t codepoint) {
    return codepoint == 0x20 || codepoint == 0x9 || codepoint == 0xD || codepoint == 0xA;
}

static void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
}

/* At the end of the input the lookahead is 0, which is no [2] Char: the
 * eof callback is asked only then. */
static bool is_at_end(TSLexer *lexer) {
    return lexer->lookahead == 0 && lexer->eof(lexer);
}

/* --- Growable storage --------------------------------------------------- */

/* Makes room for `required_count` elements of `element_size` bytes in
 * *storage. Returns false, leaving *storage alone, if the size overflows or
 * the allocator fails. */
typedef struct {
    void **storage;
    uint32_t *capacity;
    uint32_t required_count;
    size_t element_size;
} growth_request;

static bool reserve(const growth_request *request) {
    if (request->required_count <= *request->capacity) {
        return true;
    }
    uint32_t new_capacity = *request->capacity < 16 ? 16 : *request->capacity;
    while (new_capacity < request->required_count) {
        if (new_capacity > UINT32_MAX / 2) {
            return false;
        }
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / request->element_size) {
        return false;
    }
    void *grown = ts_realloc(*request->storage, (size_t)new_capacity * request->element_size);
    if (grown == NULL) {
        return false;
    }
    *request->storage = grown;
    *request->capacity = new_capacity;
    return true;
}

/* --- Tag stack ----------------------------------------------------------- */

/* FNV-1a over the parent frame index and the name bytes. */
static uint32_t frame_hash(const scanner_state *state, uint32_t parent_frame) {
    uint32_t hash = 2166136261u;
    for (int shift = 0; shift < 32; shift += 8) {
        hash = (hash ^ ((parent_frame >> shift) & 0xFFu)) * 16777619u;
    }
    for (uint32_t byte_index = 0; byte_index < state->scanned_name_byte_count; byte_index += 1) {
        hash = (hash ^ state->scanned_name[byte_index]) * 16777619u;
    }
    return hash;
}

static bool frame_matches_scanned_name(const scanner_state *state, uint32_t frame_index) {
    const tag_frame *frame = &state->frames[frame_index];
    return frame->name_byte_count == state->scanned_name_byte_count &&
           memcmp(&state->name_bytes[frame->name_offset], state->scanned_name,
                  state->scanned_name_byte_count) == 0;
}

/* Rebuilds the hash table with twice the slots. */
static bool grow_slots(scanner_state *state) {
    uint32_t new_capacity = state->slot_capacity == 0 ? 64 : state->slot_capacity * 2;
    if (new_capacity == 0) {
        return false;
    }
    /* calloc checks the multiplication for overflow. */
    uint32_t *new_slots = ts_calloc(new_capacity, sizeof(uint32_t));
    if (new_slots == NULL) {
        return false;
    }
    uint32_t mask = new_capacity - 1;
    for (uint32_t frame_index = 1; frame_index < state->frame_count; frame_index += 1) {
        uint32_t slot_index = state->frames[frame_index].hash & mask;
        while (new_slots[slot_index] != 0) {
            slot_index = (slot_index + 1) & mask;
        }
        new_slots[slot_index] = frame_index;
    }
    ts_free(state->slots);
    state->slots = new_slots;
    state->slot_capacity = new_capacity;
    return true;
}

/* Pushes the scanned name: makes the frame (current frame, scanned name)
 * current, creating it if it does not exist yet. */
static bool push_scanned_name(scanner_state *state) {
    uint32_t parent_frame = state->current_frame;
    uint32_t hash = frame_hash(state, parent_frame);

    if (state->slot_capacity != 0) {
        uint32_t mask = state->slot_capacity - 1;
        for (uint32_t slot_index = hash & mask; state->slots[slot_index] != 0;
             slot_index = (slot_index + 1) & mask) {
            uint32_t frame_index = state->slots[slot_index];
            const tag_frame *frame = &state->frames[frame_index];
            if (frame->hash == hash && frame->parent_frame == parent_frame &&
                frame_matches_scanned_name(state, frame_index)) {
                state->current_frame = frame_index;
                return true;
            }
        }
    }

    /* Keep the table at most half full. */
    if ((uint64_t)state->frame_count * 2 >= state->slot_capacity && !grow_slots(state)) {
        return false;
    }
    if (state->frame_count == UINT32_MAX ||
        state->name_byte_count > UINT32_MAX - state->scanned_name_byte_count) {
        return false;
    }
    if (!reserve(&(growth_request){
            .storage = (void **)&state->frames,
            .capacity = &state->frame_capacity,
            .required_count = state->frame_count + 1,
            .element_size = sizeof(tag_frame),
        }) ||
        !reserve(&(growth_request){
            .storage = (void **)&state->name_bytes,
            .capacity = &state->name_byte_capacity,
            .required_count = state->name_byte_count + state->scanned_name_byte_count,
            .element_size = 1,
        })) {
        return false;
    }

    uint32_t frame_index = state->frame_count;
    state->frames[frame_index] = (tag_frame){
        .parent_frame = parent_frame,
        .name_offset = state->name_byte_count,
        .name_byte_count = state->scanned_name_byte_count,
        .hash = hash,
    };
    if (state->scanned_name_byte_count > 0) {
        memcpy(&state->name_bytes[state->name_byte_count], state->scanned_name,
               state->scanned_name_byte_count);
    }
    state->name_byte_count += state->scanned_name_byte_count;
    state->frame_count += 1;

    uint32_t mask = state->slot_capacity - 1;
    uint32_t slot_index = hash & mask;
    while (state->slots[slot_index] != 0) {
        slot_index = (slot_index + 1) & mask;
    }
    state->slots[slot_index] = frame_index;
    state->current_frame = frame_index;
    return true;
}

/* Appends one codepoint, UTF-8 encoded, to the scanned name. */
static bool append_to_scanned_name(scanner_state *state, int32_t codepoint) {
    uint8_t encoded[4];
    uint32_t encoded_byte_count;
    uint32_t value = (uint32_t)codepoint;
    if (value < 0x80) {
        encoded[0] = (uint8_t)value;
        encoded_byte_count = 1;
    } else if (value < 0x800) {
        encoded[0] = (uint8_t)(0xC0 | (value >> 6));
        encoded[1] = (uint8_t)(0x80 | (value & 0x3F));
        encoded_byte_count = 2;
    } else if (value < 0x10000) {
        encoded[0] = (uint8_t)(0xE0 | (value >> 12));
        encoded[1] = (uint8_t)(0x80 | ((value >> 6) & 0x3F));
        encoded[2] = (uint8_t)(0x80 | (value & 0x3F));
        encoded_byte_count = 3;
    } else {
        encoded[0] = (uint8_t)(0xF0 | (value >> 18));
        encoded[1] = (uint8_t)(0x80 | ((value >> 12) & 0x3F));
        encoded[2] = (uint8_t)(0x80 | ((value >> 6) & 0x3F));
        encoded[3] = (uint8_t)(0x80 | (value & 0x3F));
        encoded_byte_count = 4;
    }
    if (state->scanned_name_byte_count > UINT32_MAX - encoded_byte_count ||
        !reserve(&(growth_request){
            .storage = (void **)&state->scanned_name,
            .capacity = &state->scanned_name_capacity,
            .required_count = state->scanned_name_byte_count + encoded_byte_count,
            .element_size = 1,
        })) {
        return false;
    }
    memcpy(&state->scanned_name[state->scanned_name_byte_count], encoded, encoded_byte_count);
    state->scanned_name_byte_count += encoded_byte_count;
    return true;
}

/* [5] Name, read into state->scanned_name. */
static bool scan_name(scanner_state *state, TSLexer *lexer) {
    state->scanned_name_byte_count = 0;
    if (is_at_end(lexer) || !is_name_start_character(lexer->lookahead)) {
        return false;
    }
    while (!is_at_end(lexer) && is_name_character(lexer->lookahead)) {
        if (lexer->lookahead < 0x80 &&
            state->scanned_name_byte_count < state->scanned_name_capacity) {
            /* ASCII with room left: no encoding, no growth. */
            state->scanned_name[state->scanned_name_byte_count] = (uint8_t)lexer->lookahead;
            state->scanned_name_byte_count += 1;
        } else if (!append_to_scanned_name(state, lexer->lookahead)) {
            return false;
        }
        advance(lexer);
    }
    return true;
}

/* --- Tokens -------------------------------------------------------------- */

/* [40] STag, [44] EmptyElemTag, after the '<': the '<' and the element
 * name (START_TAG_OPEN), pushed on the stack; '<' Name '>' (START_TAG_WHOLE),
 * pushed too; or '<' Name '/>' (EMPTY_ELEMENT_TAG_WHOLE), which leaves the
 * stack as it was. */
static bool scan_start_tag(scanner_state *state, TSLexer *lexer, const bool *valid_symbols) {
    if (!scan_name(state, lexer)) {
        return false;
    }
    lexer->mark_end(lexer);
    if (lexer->lookahead == '>' && valid_symbols[START_TAG_WHOLE]) {
        advance(lexer);
        lexer->mark_end(lexer);
        lexer->result_symbol = START_TAG_WHOLE;
        return push_scanned_name(state);
    }
    if (lexer->lookahead == '/' && valid_symbols[EMPTY_ELEMENT_TAG_WHOLE]) {
        advance(lexer);
        if (lexer->lookahead == '>') {
            advance(lexer);
            lexer->mark_end(lexer);
            lexer->result_symbol = EMPTY_ELEMENT_TAG_WHOLE;
            return true;
        }
    }
    /* The token ends after the name (the first mark_end). */
    lexer->result_symbol = START_TAG_OPEN;
    return push_scanned_name(state);
}

/* [42] ETag ::= '</' Name S? '>', after the '<': the name must close the
 * innermost open element (WFC: Element Type Match). */
static bool scan_end_tag(scanner_state *state, TSLexer *lexer) {
    if (lexer->lookahead != '/') {
        return false;
    }
    advance(lexer);
    if (state->current_frame == EMPTY_STACK_FRAME || !scan_name(state, lexer) ||
        !frame_matches_scanned_name(state, state->current_frame)) {
        return false;
    }
    while (is_space_character(lexer->lookahead)) {
        advance(lexer);
    }
    if (lexer->lookahead != '>') {
        return false;
    }
    advance(lexer);
    state->current_frame = state->frames[state->current_frame].parent_frame;
    lexer->result_symbol = END_TAG;
    return true;
}

/* '/>' of [44] EmptyElemTag: pops the name pushed by its start. */
static bool scan_empty_element_tag_close(scanner_state *state, TSLexer *lexer) {
    if (lexer->lookahead != '/' || state->current_frame == EMPTY_STACK_FRAME) {
        return false;
    }
    advance(lexer);
    if (lexer->lookahead != '>') {
        return false;
    }
    advance(lexer);
    state->current_frame = state->frames[state->current_frame].parent_frame;
    lexer->result_symbol = EMPTY_ELEMENT_TAG_CLOSE;
    return true;
}

/* [41] Attribute ::= Name Eq AttValue, [25] Eq ::= S? '=' S?, for a value
 * without a Reference: [10] '"' [^<&"]* '"' | "'" [^<&']* "'". A value with
 * a reference, a '<' or a character outside [2] Char is left to the
 * grammar's own rule, which reads the references (and refuses the rest). */
static bool scan_attribute_whole(scanner_state *state, TSLexer *lexer) {
    if (!scan_name(state, lexer)) {
        return false;
    }
    while (is_space_character(lexer->lookahead)) {
        advance(lexer);
    }
    if (lexer->lookahead != '=') {
        return false;
    }
    advance(lexer);
    while (is_space_character(lexer->lookahead)) {
        advance(lexer);
    }
    const int32_t quote = lexer->lookahead;
    if (quote != '"' && quote != '\'') {
        return false;
    }
    advance(lexer);
    while (!is_at_end(lexer)) {
        const int32_t character = lexer->lookahead;
        if (character == quote) {
            advance(lexer);
            lexer->result_symbol = ATTRIBUTE_WHOLE;
            return true;
        }
        if (character == '<' || character == '&' || !is_xml_character(character)) {
            return false;
        }
        advance(lexer);
    }
    return false;
}

/* [14] CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)
 * Rejects the whole run if it contains ']]>' or a character outside [2] Char. */
static bool scan_character_data(TSLexer *lexer) {
    bool has_consumed = false;
    uint32_t bracket_run = 0;
    while (!is_at_end(lexer) && lexer->lookahead != '<' && lexer->lookahead != '&') {
        int32_t character = lexer->lookahead;
        if (!is_xml_character(character) || (character == '>' && bracket_run >= 2)) {
            return false;
        }
        bracket_run = character == ']' ? bracket_run + 1 : 0;
        advance(lexer);
        has_consumed = true;
    }
    if (!has_consumed) {
        return false;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = CHARACTER_DATA;
    return true;
}

/* [20] CData ::= (Char* - (Char* ']]>' Char*)), followed by ']]>'.
 *
 * The token must end before the last two ']' of a run of brackets followed
 * by '>', but mark_end can only mark the current position. So text before
 * a bracket run is one token, and a run of three or more brackets before
 * '>' yields one-bracket tokens until two are left. The grammar accepts
 * adjacent cdata_text nodes; the tree builder joins them. */
static bool scan_cdata_text(TSLexer *lexer) {
    bool has_consumed = false;
    while (!is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == ']') {
            uint32_t bracket_run = 0;
            if (has_consumed) {
                lexer->mark_end(lexer);
            } else {
                advance(lexer);
                lexer->mark_end(lexer);
                bracket_run = 1;
            }
            while (lexer->lookahead == ']') {
                advance(lexer);
                bracket_run += 1;
            }
            if (lexer->lookahead == '>' && bracket_run >= 2) {
                if (has_consumed || bracket_run >= 3) {
                    lexer->result_symbol = CDATA_TEXT;
                    return true;
                }
                return false; /* Only the delimiter ']]>' is left. */
            }
            has_consumed = true;
            continue;
        }
        if (!is_xml_character(character)) {
            return false;
        }
        advance(lexer);
        has_consumed = true;
    }
    return false;
}

/* [15] Comment: the text between '<!--' and '-->', which contains no '--'. */
static bool scan_comment_text(TSLexer *lexer) {
    bool has_consumed = false;
    while (!is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == '-') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '-') {
                advance(lexer);
                if (lexer->lookahead == '>' && has_consumed) {
                    lexer->result_symbol = COMMENT_TEXT;
                    return true;
                }
                return false; /* '--' inside a comment, or only '-->' is left. */
            }
            has_consumed = true;
            continue;
        }
        if (!is_xml_character(character)) {
            return false;
        }
        advance(lexer);
        has_consumed = true;
    }
    return false;
}

/* [17] PITarget ::= Name - (('X' | 'x') ('M' | 'm') ('L' | 'l'))
 * The name "xml" is left to the grammar, which accepts it only as the start
 * of the XML declaration. */
static bool scan_processing_instruction_target(scanner_state *state, TSLexer *lexer) {
    if (!scan_name(state, lexer)) {
        return false;
    }
    const uint8_t *name = state->scanned_name;
    if (state->scanned_name_byte_count == 3 && (name[0] | 0x20) == 'x' &&
        (name[1] | 0x20) == 'm' && (name[2] | 0x20) == 'l') {
        return false;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = PROCESSING_INSTRUCTION_TARGET;
    return true;
}

/* [16] PI: the content after the target's whitespace, up to '?>'. */
static bool scan_processing_instruction_content(TSLexer *lexer) {
    bool has_consumed = false;
    while (!is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == '?') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '>') {
                if (!has_consumed) {
                    return false;
                }
                lexer->result_symbol = PROCESSING_INSTRUCTION_CONTENT;
                return true;
            }
            has_consumed = true;
            continue;
        }
        if (!is_xml_character(character)) {
            return false;
        }
        advance(lexer);
        has_consumed = true;
    }
    return false;
}

/* [69] PEReference inside a markup declaration of the external subset
 * (2.8): the whole declaration, from '<!' to its closing '>', when a
 * parameter-entity reference occurs outside its quoted literals. Such a
 * declaration matches its production only after the tree builder replaces
 * the references (4.4.8, Included as PE), so the grammar sees one token.
 * Without a reference the scanner offers nothing and the grammar parses
 * the declaration itself. */
static bool scan_unexpanded_declaration(TSLexer *lexer) {
    if (lexer->lookahead != '<') {
        return false;
    }
    advance(lexer);
    if (lexer->lookahead != '!') {
        return false;
    }
    advance(lexer);
    /* The keyword: its first two letters decide which one it must be. */
    const char *rest = NULL;
    if (lexer->lookahead == 'E') {
        advance(lexer);
        rest = lexer->lookahead == 'L' ? "LEMENT" : lexer->lookahead == 'N' ? "NTITY" : NULL;
    } else if (lexer->lookahead == 'A') {
        rest = "ATTLIST";
    } else if (lexer->lookahead == 'N') {
        rest = "NOTATION";
    }
    if (rest == NULL) {
        return false;
    }
    for (const char *letter = rest; *letter != '\0'; letter += 1) {
        if (lexer->lookahead != *letter) {
            return false;
        }
        advance(lexer);
    }
    bool has_reference = false;
    int32_t quote = 0;
    while (!is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (!is_xml_character(character)) {
            return false;
        }
        if (quote != 0) {
            if (character == quote) {
                quote = 0;
            }
            advance(lexer);
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
            advance(lexer);
            continue;
        }
        if (character == '>') {
            if (!has_reference) {
                return false;
            }
            advance(lexer);
            lexer->mark_end(lexer);
            lexer->result_symbol = UNEXPANDED_DECLARATION;
            return true;
        }
        if (character == '<') {
            break;
        }
        advance(lexer);
        if (character == '%' && is_name_start_character(lexer->lookahead)) {
            while (!is_at_end(lexer) && is_name_character(lexer->lookahead)) {
                advance(lexer);
            }
            if (lexer->lookahead == ';') {
                has_reference = true;
            }
        }
    }
    /* No '>' before the next markup or the end: the '>' may come from a
     * parameter entity (VC: Proper Declaration/PE Nesting, 2.8), which only
     * validity forbids. The tree builder decides once it has expanded. */
    if (!has_reference || quote != 0) {
        return false;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = UNEXPANDED_DECLARATION;
    return true;
}

/* [65] Ignore ::= Char* - (Char* ('<![' | ']]>') Char*): text of an
 * ignored section up to the next '<![' or ']]>', which the grammar reads.
 * As for CDATA text, a run of brackets before '>' is returned in pieces,
 * because the token cannot end two characters before the position. */
static bool scan_ignored_text(TSLexer *lexer) {
    bool has_consumed = false;
    while (!is_at_end(lexer)) {
        int32_t character = lexer->lookahead;
        if (character == '<') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '!') {
                advance(lexer);
                if (lexer->lookahead == '[') {
                    if (has_consumed) {
                        lexer->result_symbol = IGNORED_TEXT;
                        return true;
                    }
                    return false;
                }
            }
            has_consumed = true;
            continue;
        }
        if (character == ']') {
            uint32_t bracket_run = 0;
            if (has_consumed) {
                lexer->mark_end(lexer);
            } else {
                advance(lexer);
                lexer->mark_end(lexer);
                bracket_run = 1;
            }
            while (lexer->lookahead == ']') {
                advance(lexer);
                bracket_run += 1;
            }
            if (lexer->lookahead == '>' && bracket_run >= 2) {
                if (has_consumed || bracket_run >= 3) {
                    lexer->result_symbol = IGNORED_TEXT;
                    return true;
                }
                return false; /* Only the delimiter ']]>' is left. */
            }
            has_consumed = true;
            continue;
        }
        if (!is_xml_character(character)) {
            return false;
        }
        advance(lexer);
        has_consumed = true;
    }
    if (has_consumed) {
        lexer->mark_end(lexer);
        lexer->result_symbol = IGNORED_TEXT;
        return true;
    }
    return false;
}

/* --- tree-sitter entry points --------------------------------------------- */

void *tree_sitter_xml_external_scanner_create(void) {
    scanner_state *state = ts_calloc(1, sizeof(scanner_state));
    if (state == NULL) {
        return NULL;
    }
    if (!reserve(&(growth_request){
            .storage = (void **)&state->frames,
            .capacity = &state->frame_capacity,
            .required_count = 1,
            .element_size = sizeof(tag_frame),
        })) {
        ts_free(state);
        return NULL;
    }
    state->frames[EMPTY_STACK_FRAME] = (tag_frame){0};
    state->frame_count = 1;
    state->current_frame = EMPTY_STACK_FRAME;
    return state;
}

void tree_sitter_xml_external_scanner_destroy(void *payload) {
    scanner_state *state = payload;
    if (state == NULL) {
        return;
    }
    ts_free(state->frames);
    ts_free(state->name_bytes);
    ts_free(state->slots);
    ts_free(state->scanned_name);
    ts_free(state);
}

unsigned tree_sitter_xml_external_scanner_serialize(void *payload, char *buffer) {
    const scanner_state *state = payload;
    if (state == NULL || state->current_frame == EMPTY_STACK_FRAME) {
        return 0;
    }
    memcpy(buffer, &state->current_frame, sizeof state->current_frame);
    return sizeof state->current_frame;
}

void tree_sitter_xml_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    scanner_state *state = payload;
    if (state == NULL) {
        return;
    }
    state->current_frame = EMPTY_STACK_FRAME;
    if (length == sizeof state->current_frame) {
        uint32_t frame_index;
        memcpy(&frame_index, buffer, sizeof frame_index);
        if (frame_index < state->frame_count) {
            state->current_frame = frame_index;
        }
    }
}

bool tree_sitter_xml_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    scanner_state *state = payload;
    /* In error recovery every symbol is valid; no token is offered then, so
     * recovery never makes broken input look well-formed. */
    if (state == NULL || valid_symbols[ERROR_SENTINEL]) {
        return false;
    }
    if (valid_symbols[UNEXPANDED_DECLARATION]) {
        return scan_unexpanded_declaration(lexer);
    }
    if (valid_symbols[IGNORED_TEXT]) {
        return scan_ignored_text(lexer);
    }
    if (lexer->lookahead == '<' && (valid_symbols[START_TAG_OPEN] || valid_symbols[END_TAG])) {
        advance(lexer);
        if (lexer->lookahead == '/') {
            return valid_symbols[END_TAG] && scan_end_tag(state, lexer);
        }
        /* Not a tag ('<!', '<?'): the grammar's own tokens. */
        return valid_symbols[START_TAG_OPEN] && scan_start_tag(state, lexer, valid_symbols);
    }
    if (valid_symbols[EMPTY_ELEMENT_TAG_CLOSE] && lexer->lookahead == '/') {
        return scan_empty_element_tag_close(state, lexer);
    }
    if (valid_symbols[ATTRIBUTE_WHOLE]) {
        return scan_attribute_whole(state, lexer);
    }
    if (valid_symbols[CHARACTER_DATA]) {
        return scan_character_data(lexer);
    }
    if (valid_symbols[CDATA_TEXT]) {
        return scan_cdata_text(lexer);
    }
    if (valid_symbols[COMMENT_TEXT]) {
        return scan_comment_text(lexer);
    }
    if (valid_symbols[PROCESSING_INSTRUCTION_TARGET]) {
        return scan_processing_instruction_target(state, lexer);
    }
    if (valid_symbols[PROCESSING_INSTRUCTION_CONTENT]) {
        return scan_processing_instruction_content(lexer);
    }
    return false;
}
