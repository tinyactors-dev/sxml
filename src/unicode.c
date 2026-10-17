/*
 * Unicode normalization, grapheme clusters and character reference names
 * (src/unicode_internal.h, docs/DECISIONS.md D107) over the generated data
 * of src/unicode_tables.c.
 *
 * Golden file: src/regex_unicode.c (lookups in sorted runs), src/memory.c
 * (allocation and overflow checks).
 *
 * Normalization follows UAX #15 (Unicode Normalization Forms) section 1.2
 * and the sample code of its section 9 ("Detecting Normalization Forms"
 * aside): every codepoint is replaced by its full decomposition, each run
 * of non-starters is put in canonical order (The Unicode Standard 16.0,
 * section 3.11, "Canonical Ordering Algorithm"), and the composed forms
 * then join starter and following codepoints into primary composites
 * unless blocked ("Canonical Composition Algorithm", same section).
 *
 * Grapheme clusters follow UAX #29 (Unicode Text Segmentation), section
 * 3.1.1, "Grapheme Cluster Boundary Rules", for Unicode 16.0.
 */
#include "unicode_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Lookups */

/* Hangul syllables (The Unicode Standard 16.0, section 3.12, "Conjoining
   Jamo Behavior"). */
#define SXML_UNICODE_HANGUL_S_BASE 0xAC00U
#define SXML_UNICODE_HANGUL_L_BASE 0x1100U
#define SXML_UNICODE_HANGUL_V_BASE 0x1161U
#define SXML_UNICODE_HANGUL_T_BASE 0x11A7U
#define SXML_UNICODE_HANGUL_L_COUNT 19U
#define SXML_UNICODE_HANGUL_V_COUNT 21U
#define SXML_UNICODE_HANGUL_T_COUNT 28U
#define SXML_UNICODE_HANGUL_N_COUNT (SXML_UNICODE_HANGUL_V_COUNT * SXML_UNICODE_HANGUL_T_COUNT)
#define SXML_UNICODE_HANGUL_S_COUNT (SXML_UNICODE_HANGUL_L_COUNT * SXML_UNICODE_HANGUL_N_COUNT)

/* The packing of the generated runs and slices. */
#define SXML_UNICODE_RUN_VALUE_BIT_COUNT 8U
#define SXML_UNICODE_RUN_VALUE_MASK 0xFFU
#define SXML_UNICODE_SLICE_COUNT_BIT_COUNT 5U
#define SXML_UNICODE_SLICE_COUNT_MASK 0x1FU
#define SXML_UNICODE_GRAPHEME_BREAK_MASK 0x0FU
#define SXML_UNICODE_CONJUNCT_BREAK_SHIFT 4U
#define SXML_UNICODE_CONJUNCT_BREAK_MASK 0x03U
#define SXML_UNICODE_EXTENDED_PICTOGRAPHIC_BIT 0x40U

static bool is_hangul_syllable(uint32_t codepoint) {
    return codepoint >= SXML_UNICODE_HANGUL_S_BASE &&
           codepoint < SXML_UNICODE_HANGUL_S_BASE + SXML_UNICODE_HANGUL_S_COUNT;
}

/* The value of the last run that starts at or before codepoint; the
   first run starts at 0. */
typedef struct run_table {
    const uint32_t *runs;
    size_t run_count;
} run_table;

static uint32_t find_run_value(run_table table, uint32_t codepoint) {
    size_t low_index = 0;
    size_t high_index = table.run_count;
    while (high_index - low_index > 1) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        if ((table.runs[middle_index] >> SXML_UNICODE_RUN_VALUE_BIT_COUNT) <= codepoint) {
            low_index = middle_index;
        } else {
            high_index = middle_index;
        }
    }
    return table.runs[low_index] & SXML_UNICODE_RUN_VALUE_MASK;
}

uint8_t sxml_unicode_get_combining_class(uint32_t codepoint) {
    /* Below U+0300 every codepoint is a starter. */
    if (codepoint < 0x300U) {
        return 0;
    }
    return (uint8_t)find_run_value(
        (run_table){.runs = sxml_unicode_combining_class_runs,
                    .run_count = sxml_unicode_combining_class_run_count},
        codepoint);
}

/* A table of decompositions sorted by codepoint. */
typedef struct decomposition_table {
    const sxml_unicode_decomposition_entry *entries;
    size_t entry_count;
} decomposition_table;

static const sxml_unicode_decomposition_entry *find_decomposition(decomposition_table table,
                                                                  uint32_t codepoint) {
    size_t low_index = 0;
    size_t high_index = table.entry_count;
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const uint32_t middle_codepoint = table.entries[middle_index].codepoint;
        if (middle_codepoint == codepoint) {
            return &table.entries[middle_index];
        }
        if (middle_codepoint < codepoint) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return NULL;
}

void sxml_unicode_decompose(sxml_unicode_decomposition_request request,
                            sxml_unicode_decomposition *decomposition) {
    const uint32_t codepoint = request.codepoint;
    if (is_hangul_syllable(codepoint)) {
        /* Section 3.12, "Hangul Syllable Decomposition"; canonical, so
           both kinds agree. */
        const uint32_t syllable_index = codepoint - SXML_UNICODE_HANGUL_S_BASE;
        const uint32_t trailing_index = syllable_index % SXML_UNICODE_HANGUL_T_COUNT;
        decomposition->codepoints[0] =
            SXML_UNICODE_HANGUL_L_BASE + syllable_index / SXML_UNICODE_HANGUL_N_COUNT;
        decomposition->codepoints[1] =
            SXML_UNICODE_HANGUL_V_BASE +
            (syllable_index % SXML_UNICODE_HANGUL_N_COUNT) / SXML_UNICODE_HANGUL_T_COUNT;
        decomposition->codepoint_count = 2;
        if (trailing_index != 0) {
            decomposition->codepoints[2] = SXML_UNICODE_HANGUL_T_BASE + trailing_index;
            decomposition->codepoint_count = 3;
        }
        return;
    }
    const sxml_unicode_decomposition_entry *entry = NULL;
    if (codepoint >= 0xA0U) {
        if (request.decomposition_kind == SXML_UNICODE_DECOMPOSITION_KIND_COMPATIBILITY) {
            entry = find_decomposition(
                (decomposition_table){
                    .entries = sxml_unicode_compatibility_decompositions,
                    .entry_count = sxml_unicode_compatibility_decomposition_count},
                codepoint);
        }
        if (entry == NULL) {
            entry = find_decomposition(
                (decomposition_table){.entries = sxml_unicode_canonical_decompositions,
                                      .entry_count = sxml_unicode_canonical_decomposition_count},
                codepoint);
        }
    }
    if (entry == NULL) {
        decomposition->codepoints[0] = codepoint;
        decomposition->codepoint_count = 1;
        return;
    }
    const size_t first_index = entry->packed_slice >> SXML_UNICODE_SLICE_COUNT_BIT_COUNT;
    const size_t codepoint_count = entry->packed_slice & SXML_UNICODE_SLICE_COUNT_MASK;
    memcpy(decomposition->codepoints, &sxml_unicode_decomposition_codepoints[first_index],
           codepoint_count * sizeof(uint32_t));
    decomposition->codepoint_count = codepoint_count;
}

bool sxml_unicode_compose(sxml_unicode_codepoint_pair pair, uint32_t *composite_codepoint) {
    const uint32_t first = pair.first_codepoint;
    const uint32_t second = pair.second_codepoint;
    /* Section 3.12, "Hangul Syllable Composition": L V to LV, LV T to
       LVT. */
    if (first >= SXML_UNICODE_HANGUL_L_BASE &&
        first < SXML_UNICODE_HANGUL_L_BASE + SXML_UNICODE_HANGUL_L_COUNT &&
        second >= SXML_UNICODE_HANGUL_V_BASE &&
        second < SXML_UNICODE_HANGUL_V_BASE + SXML_UNICODE_HANGUL_V_COUNT) {
        *composite_codepoint =
            SXML_UNICODE_HANGUL_S_BASE +
            ((first - SXML_UNICODE_HANGUL_L_BASE) * SXML_UNICODE_HANGUL_V_COUNT +
             (second - SXML_UNICODE_HANGUL_V_BASE)) *
                SXML_UNICODE_HANGUL_T_COUNT;
        return true;
    }
    if (is_hangul_syllable(first) &&
        (first - SXML_UNICODE_HANGUL_S_BASE) % SXML_UNICODE_HANGUL_T_COUNT == 0 &&
        second > SXML_UNICODE_HANGUL_T_BASE &&
        second < SXML_UNICODE_HANGUL_T_BASE + SXML_UNICODE_HANGUL_T_COUNT) {
        *composite_codepoint = first + (second - SXML_UNICODE_HANGUL_T_BASE);
        return true;
    }
    size_t low_index = 0;
    size_t high_index = sxml_unicode_composition_count;
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const sxml_unicode_composition *middle = &sxml_unicode_compositions[middle_index];
        if (middle->first_codepoint == first && middle->second_codepoint == second) {
            *composite_codepoint = middle->composite_codepoint;
            return true;
        }
        if (middle->first_codepoint < first ||
            (middle->first_codepoint == first && middle->second_codepoint < second)) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return false;
}

bool sxml_unicode_is_composing_character(uint32_t codepoint) {
    if (sxml_unicode_get_combining_class(codepoint) != 0) {
        return true;
    }
    /* The vowel and trailing jamo are the second codepoints of the
       canonical decompositions of the Hangul syllables (section 3.12). */
    if ((codepoint >= SXML_UNICODE_HANGUL_V_BASE &&
         codepoint < SXML_UNICODE_HANGUL_V_BASE + SXML_UNICODE_HANGUL_V_COUNT) ||
        (codepoint > SXML_UNICODE_HANGUL_T_BASE &&
         codepoint < SXML_UNICODE_HANGUL_T_BASE + SXML_UNICODE_HANGUL_T_COUNT)) {
        return true;
    }
    size_t low_index = 0;
    size_t high_index = sxml_unicode_composing_starter_count;
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const uint32_t middle_codepoint = sxml_unicode_composing_starters[middle_index];
        if (middle_codepoint == codepoint) {
            return true;
        }
        if (middle_codepoint < codepoint) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return false;
}

/* ------------------------------------------------------- Normalization */

/* Codepoints being normalized, in allocator memory. */
typedef struct codepoint_array {
    sxml_allocator allocator;
    uint32_t *codepoints;
    size_t codepoint_count;
    size_t codepoint_capacity;
} codepoint_array;

/* Makes room for at least minimum_capacity codepoints (allocating anew
   and copying, D22). */
static sxml_status reserve_codepoints(codepoint_array *array, size_t minimum_capacity) {
    if (minimum_capacity <= array->codepoint_capacity) {
        return SXML_STATUS_OK;
    }
    size_t new_capacity = array->codepoint_capacity < 32 ? 32 : array->codepoint_capacity;
    while (new_capacity < minimum_capacity) {
        sxml_status status = sxml_size_multiply(new_capacity, 2, &new_capacity);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(new_capacity, sizeof(uint32_t), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            array->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(uint32_t)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (array->codepoint_count > 0) {
        memcpy(memory, array->codepoints, array->codepoint_count * sizeof(uint32_t));
    }
    sxml_allocator_release(array->allocator, array->codepoints);
    array->codepoints = memory;
    array->codepoint_capacity = new_capacity;
    return SXML_STATUS_OK;
}

static void release_codepoints(codepoint_array *array) {
    sxml_allocator_release(array->allocator, array->codepoints);
    array->codepoints = NULL;
    array->codepoint_count = 0;
    array->codepoint_capacity = 0;
}

/* Codepoints to append, borrowed. */
typedef struct codepoint_view {
    const uint32_t *codepoints;
    size_t codepoint_count;
} codepoint_view;

static sxml_status append_codepoints(codepoint_array *array, codepoint_view view) {
    size_t needed_capacity = 0;
    sxml_status status = sxml_size_add(array->codepoint_count, view.codepoint_count,
                                       &needed_capacity);
    if (status == SXML_STATUS_OK) {
        status = reserve_codepoints(array, needed_capacity);
    }
    if (status == SXML_STATUS_OK && view.codepoint_count > 0) {
        if (array->codepoints == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        memcpy(&array->codepoints[array->codepoint_count], view.codepoints,
               view.codepoint_count * sizeof(uint32_t));
        array->codepoint_count += view.codepoint_count;
    }
    return status;
}

/* Appends the full decompositions of the codepoints of input (UAX #15,
   section 1.2: "Decomposition"), after a space when the form is
   FULLY-NORMALIZED and input starts with a composing character. */
static sxml_status decompose_input(const sxml_unicode_normalization_request *request,
                                   codepoint_array *array) {
    const sxml_unicode_decomposition_kind decomposition_kind =
        request->normalization_form == SXML_UNICODE_NORMALIZATION_FORM_NFKC ||
                request->normalization_form == SXML_UNICODE_NORMALIZATION_FORM_NFKD
            ? SXML_UNICODE_DECOMPOSITION_KIND_COMPATIBILITY
            : SXML_UNICODE_DECOMPOSITION_KIND_CANONICAL;
    size_t first_capacity = 0;
    sxml_status status = sxml_size_add(request->input.codepoint_count, 1, &first_capacity);
    if (status == SXML_STATUS_OK) {
        status = reserve_codepoints(array, first_capacity);
    }
    sxml_codepoint_iterator iterator = {.string = request->input, .byte_offset = 0};
    uint32_t codepoint = 0;
    bool is_first = true;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        if (is_first &&
            request->normalization_form == SXML_UNICODE_NORMALIZATION_FORM_FULLY_NORMALIZED &&
            sxml_unicode_is_composing_character(codepoint)) {
            /* F&O 4.0, fn:normalize-unicode: "if the first character in
               the string is a composing character, prepend a single
               space". */
            static const uint32_t space[] = {' '};
            status = append_codepoints(array,
                                       (codepoint_view){.codepoints = space, .codepoint_count = 1});
        }
        is_first = false;
        sxml_unicode_decomposition decomposition;
        sxml_unicode_decompose(
            (sxml_unicode_decomposition_request){.codepoint = codepoint,
                                                 .decomposition_kind = decomposition_kind},
            &decomposition);
        if (status == SXML_STATUS_OK) {
            status = append_codepoints(
                array, (codepoint_view){.codepoints = decomposition.codepoints,
                                        .codepoint_count = decomposition.codepoint_count});
        }
    }
    return status;
}

/* Runs of non-starters up to this length are sorted by insertion. */
#define SXML_UNICODE_INSERTION_SORT_LENGTH 16U
#define SXML_UNICODE_COMBINING_CLASS_COUNT 256U

/* A run of non-starters, codepoints[first_index] up to before
   end_index. */
typedef struct codepoint_run {
    size_t first_index;
    size_t end_index;
} codepoint_run;

static void sort_run_by_insertion(uint32_t *codepoints, codepoint_run run) {
    for (size_t sorted_end = run.first_index + 1; sorted_end < run.end_index; sorted_end += 1) {
        const uint32_t moving = codepoints[sorted_end];
        const uint8_t moving_class = sxml_unicode_get_combining_class(moving);
        size_t position = sorted_end;
        while (position > run.first_index &&
               sxml_unicode_get_combining_class(codepoints[position - 1]) > moving_class) {
            codepoints[position] = codepoints[position - 1];
            position -= 1;
        }
        codepoints[position] = moving;
    }
}

/* A stable counting sort by combining class through scratch (as long as
   the run). */
typedef struct counting_sort {
    uint32_t *codepoints;
    uint32_t *scratch;
} counting_sort;

static void sort_run_by_counting(counting_sort sort, codepoint_run run) {
    size_t starts[SXML_UNICODE_COMBINING_CLASS_COUNT];
    memset(starts, 0, sizeof(starts));
    for (size_t position = run.first_index; position < run.end_index; position += 1) {
        starts[sxml_unicode_get_combining_class(sort.codepoints[position])] += 1;
    }
    size_t total = 0;
    for (size_t class_index = 0; class_index < SXML_UNICODE_COMBINING_CLASS_COUNT;
         class_index += 1) {
        const size_t class_count = starts[class_index];
        starts[class_index] = total;
        total += class_count;
    }
    for (size_t position = run.first_index; position < run.end_index; position += 1) {
        const uint32_t codepoint = sort.codepoints[position];
        const uint8_t combining_class = sxml_unicode_get_combining_class(codepoint);
        sort.scratch[starts[combining_class]] = codepoint;
        starts[combining_class] += 1;
    }
    memcpy(&sort.codepoints[run.first_index], sort.scratch,
           (run.end_index - run.first_index) * sizeof(uint32_t));
}

/* The Canonical Ordering Algorithm (The Unicode Standard 16.0, section
   3.11, D109): each maximal run of non-starters sorted stably by
   combining class. */
static sxml_status order_canonically(codepoint_array *array) {
    codepoint_array scratch = {.allocator = array->allocator};
    sxml_status status = SXML_STATUS_OK;
    size_t position = 0;
    while (status == SXML_STATUS_OK && position < array->codepoint_count) {
        if (sxml_unicode_get_combining_class(array->codepoints[position]) == 0) {
            position += 1;
            continue;
        }
        const size_t first_index = position;
        while (position < array->codepoint_count &&
               sxml_unicode_get_combining_class(array->codepoints[position]) != 0) {
            position += 1;
        }
        const codepoint_run run = {.first_index = first_index, .end_index = position};
        if (position - first_index <= SXML_UNICODE_INSERTION_SORT_LENGTH) {
            sort_run_by_insertion(array->codepoints, run);
            continue;
        }
        status = reserve_codepoints(&scratch, position - first_index);
        if (status == SXML_STATUS_OK) {
            sort_run_by_counting(
                (counting_sort){.codepoints = array->codepoints, .scratch = scratch.codepoints},
                run);
        }
    }
    release_codepoints(&scratch);
    return status;
}

/* The Canonical Composition Algorithm (section 3.11, D117; the sample
   code of UAX #15): a codepoint joins the last starter when it is not
   blocked, that is when no codepoint between them has a combining class
   0 or at least its own. */
static void compose_canonically(codepoint_array *array) {
    if (array->codepoint_count == 0) {
        return;
    }
    uint32_t *codepoints = array->codepoints;
    size_t starter_index = 0;
    uint32_t starter = codepoints[0];
    /* 256: the first codepoint is no starter, so nothing composes with
       it. */
    unsigned last_class = sxml_unicode_get_combining_class(starter);
    if (last_class != 0) {
        last_class = SXML_UNICODE_COMBINING_CLASS_COUNT;
    }
    size_t write_index = 1;
    for (size_t read_index = 1; read_index < array->codepoint_count; read_index += 1) {
        const uint32_t codepoint = codepoints[read_index];
        const unsigned combining_class = sxml_unicode_get_combining_class(codepoint);
        uint32_t composite = 0;
        if ((last_class < combining_class || last_class == 0) &&
            sxml_unicode_compose((sxml_unicode_codepoint_pair){.first_codepoint = starter,
                                                               .second_codepoint = codepoint},
                                 &composite)) {
            codepoints[starter_index] = composite;
            starter = composite;
            continue;
        }
        if (combining_class == 0) {
            starter_index = write_index;
            starter = codepoint;
        }
        last_class = combining_class;
        codepoints[write_index] = codepoint;
        write_index += 1;
    }
    array->codepoint_count = write_index;
}

static size_t encoded_byte_count(uint32_t codepoint) {
    if (codepoint < 0x80U) {
        return 1;
    }
    if (codepoint < 0x800U) {
        return 2;
    }
    return codepoint < 0x10000U ? 3 : 4;
}

/* UTF-8 being written into memory of a known size. */
typedef struct utf8_output {
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_offset;
} utf8_output;

/* Writes codepoint as UTF-8 (RFC 3629) at output->byte_offset, which has
   room for it (the caller counted), and advances it. */
static void encode_codepoint(uint32_t codepoint, utf8_output *output) {
    uint8_t *bytes = &output->bytes[output->byte_offset];
    const size_t byte_count = encoded_byte_count(codepoint);
    switch (byte_count) {
    case 1:
        bytes[0] = (uint8_t)codepoint;
        break;
    case 2:
        bytes[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
        bytes[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        break;
    case 3:
        bytes[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
        bytes[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        break;
    default:
        bytes[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
        bytes[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
        bytes[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        break;
    }
    output->byte_offset += byte_count;
}

/* Copies bytes into a new text. */
static sxml_status make_text(sxml_allocator allocator, sxml_byte_view bytes,
                             sxml_unicode_normalized_text *text) {
    void *memory = NULL;
    if (bytes.byte_count > 0) {
        sxml_status status = sxml_allocator_allocate(
            allocator, (sxml_allocation_request){.byte_count = bytes.byte_count, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(memory, bytes.bytes, bytes.byte_count);
    }
    *text = (sxml_unicode_normalized_text){
        .allocator = allocator, .bytes = memory, .byte_count = bytes.byte_count};
    return SXML_STATUS_OK;
}

static sxml_status encode_codepoints(const codepoint_array *array,
                                     sxml_unicode_normalized_text *text) {
    size_t byte_count = 0;
    for (size_t position = 0; position < array->codepoint_count; position += 1) {
        /* Never overflows: the array itself holds four bytes per
           codepoint. */
        byte_count += encoded_byte_count(array->codepoints[position]);
    }
    void *memory = NULL;
    if (byte_count > 0) {
        sxml_status status = sxml_allocator_allocate(
            array->allocator, (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        utf8_output output = {.bytes = memory, .byte_count = byte_count, .byte_offset = 0};
        for (size_t position = 0; position < array->codepoint_count; position += 1) {
            encode_codepoint(array->codepoints[position], &output);
        }
    }
    *text = (sxml_unicode_normalized_text){
        .allocator = array->allocator, .bytes = memory, .byte_count = byte_count};
    return SXML_STATUS_OK;
}

static bool is_all_ascii(sxml_byte_view bytes) {
    for (size_t byte_index = 0; byte_index < bytes.byte_count; byte_index += 1) {
        if (bytes.bytes[byte_index] >= 0x80U) {
            return false;
        }
    }
    return true;
}

sxml_status sxml_unicode_normalize(const sxml_unicode_normalization_request *request,
                                   sxml_unicode_normalized_text *text) {
    if (request == NULL || text == NULL || sxml_allocator_is_null(request->allocator) ||
        (unsigned)request->normalization_form >
            (unsigned)SXML_UNICODE_NORMALIZATION_FORM_FULLY_NORMALIZED) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* ASCII is in every form: no ASCII codepoint has a decomposition, a
       combining class or a composite, nor is one a composing
       character. */
    if (request->input.is_ascii || is_all_ascii(request->input.utf8)) {
        return make_text(request->allocator, request->input.utf8, text);
    }
    codepoint_array array = {.allocator = request->allocator};
    sxml_status status = decompose_input(request, &array);
    if (status == SXML_STATUS_OK) {
        status = order_canonically(&array);
    }
    if (status == SXML_STATUS_OK &&
        request->normalization_form != SXML_UNICODE_NORMALIZATION_FORM_NFD &&
        request->normalization_form != SXML_UNICODE_NORMALIZATION_FORM_NFKD) {
        compose_canonically(&array);
    }
    if (status == SXML_STATUS_OK) {
        status = encode_codepoints(&array, text);
    }
    release_codepoints(&array);
    return status;
}

sxml_byte_view sxml_unicode_normalized_text_view(const sxml_unicode_normalized_text *text) {
    return (sxml_byte_view){.bytes = text->bytes, .byte_count = text->byte_count};
}

void sxml_unicode_normalized_text_release(sxml_unicode_normalized_text *text) {
    if (text->bytes != NULL) {
        sxml_allocator_release(text->allocator, text->bytes);
    }
    text->bytes = NULL;
    text->byte_count = 0;
}

/* --------------------------------------------------- Grapheme clusters */

void sxml_unicode_get_grapheme_properties(uint32_t codepoint,
                                          sxml_unicode_grapheme_properties *properties) {
    const uint32_t value = find_run_value(
        (run_table){.runs = sxml_unicode_grapheme_runs,
                    .run_count = sxml_unicode_grapheme_run_count},
        codepoint);
    *properties = (sxml_unicode_grapheme_properties){
        .grapheme_break = (sxml_unicode_grapheme_break)(value & SXML_UNICODE_GRAPHEME_BREAK_MASK),
        .conjunct_break =
            (sxml_unicode_conjunct_break)((value >> SXML_UNICODE_CONJUNCT_BREAK_SHIFT) &
                                          SXML_UNICODE_CONJUNCT_BREAK_MASK),
        .is_extended_pictographic = (value & SXML_UNICODE_EXTENDED_PICTOGRAPHIC_BIT) != 0,
    };
}

/* How far the sequences of rules GB9c and GB11 have come. */
typedef enum sequence_progress {
    /* No such sequence ends at the previous codepoint. */
    SXML_UNICODE_SEQUENCE_NONE,
    /* GB11: \p{Extended_Pictographic} Extend*; GB9c: a consonant and
       extends or linkers, no linker yet. */
    SXML_UNICODE_SEQUENCE_STARTED,
    /* GB11: ... ZWJ; GB9c: a linker seen. */
    SXML_UNICODE_SEQUENCE_READY,
} sequence_progress;

/* What the rules need to know about the cluster so far. */
typedef struct grapheme_state {
    sxml_unicode_grapheme_break previous_break;
    /* Regional indicators that end at the previous codepoint (GB12,
       GB13). */
    size_t regional_indicator_count;
    sequence_progress emoji_progress;
    sequence_progress conjunct_progress;
} grapheme_state;

static bool is_control_break(sxml_unicode_grapheme_break grapheme_break) {
    return grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_CONTROL ||
           grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_CR ||
           grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_LF;
}

/* True when the rules of UAX #29, section 3.1.1, put a boundary between
   the cluster so far and a codepoint with next's properties. */
static bool is_boundary(const grapheme_state *state, sxml_unicode_grapheme_properties next) {
    const sxml_unicode_grapheme_break previous = state->previous_break;
    const sxml_unicode_grapheme_break following = next.grapheme_break;
    /* GB3: CR x LF. */
    if (previous == SXML_UNICODE_GRAPHEME_BREAK_CR && following == SXML_UNICODE_GRAPHEME_BREAK_LF) {
        return false;
    }
    /* GB4, GB5: break after and before controls. */
    if (is_control_break(previous) || is_control_break(following)) {
        return true;
    }
    /* GB6 to GB8: Hangul syllable sequences. */
    if (previous == SXML_UNICODE_GRAPHEME_BREAK_L &&
        (following == SXML_UNICODE_GRAPHEME_BREAK_L || following == SXML_UNICODE_GRAPHEME_BREAK_V ||
         following == SXML_UNICODE_GRAPHEME_BREAK_LV ||
         following == SXML_UNICODE_GRAPHEME_BREAK_LVT)) {
        return false;
    }
    if ((previous == SXML_UNICODE_GRAPHEME_BREAK_LV || previous == SXML_UNICODE_GRAPHEME_BREAK_V) &&
        (following == SXML_UNICODE_GRAPHEME_BREAK_V || following == SXML_UNICODE_GRAPHEME_BREAK_T)) {
        return false;
    }
    if ((previous == SXML_UNICODE_GRAPHEME_BREAK_LVT ||
         previous == SXML_UNICODE_GRAPHEME_BREAK_T) &&
        following == SXML_UNICODE_GRAPHEME_BREAK_T) {
        return false;
    }
    /* GB9, GB9a, GB9b: extends, ZWJ and spacing marks join what comes
       before, prepends what comes after. */
    if (following == SXML_UNICODE_GRAPHEME_BREAK_EXTEND ||
        following == SXML_UNICODE_GRAPHEME_BREAK_ZWJ ||
        following == SXML_UNICODE_GRAPHEME_BREAK_SPACING_MARK ||
        previous == SXML_UNICODE_GRAPHEME_BREAK_PREPEND) {
        return false;
    }
    /* GB9c: Consonant [Extend Linker]* Linker [Extend Linker]* x
       Consonant. */
    if (state->conjunct_progress == SXML_UNICODE_SEQUENCE_READY &&
        next.conjunct_break == SXML_UNICODE_CONJUNCT_BREAK_CONSONANT) {
        return false;
    }
    /* GB11: \p{Extended_Pictographic} Extend* ZWJ x
       \p{Extended_Pictographic}. */
    if (state->emoji_progress == SXML_UNICODE_SEQUENCE_READY && next.is_extended_pictographic) {
        return false;
    }
    /* GB12, GB13: regional indicators pair up. */
    if (previous == SXML_UNICODE_GRAPHEME_BREAK_REGIONAL_INDICATOR &&
        following == SXML_UNICODE_GRAPHEME_BREAK_REGIONAL_INDICATOR &&
        state->regional_indicator_count % 2 == 1) {
        return false;
    }
    /* GB999. */
    return true;
}

/* Takes a codepoint with properties into the cluster. */
static void advance_state(grapheme_state *state, sxml_unicode_grapheme_properties properties) {
    const sxml_unicode_grapheme_break grapheme_break = properties.grapheme_break;
    state->regional_indicator_count =
        grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_REGIONAL_INDICATOR
            ? state->regional_indicator_count + 1
            : 0;
    if (properties.is_extended_pictographic ||
        (state->emoji_progress == SXML_UNICODE_SEQUENCE_STARTED &&
         grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_EXTEND)) {
        state->emoji_progress = SXML_UNICODE_SEQUENCE_STARTED;
    } else if (state->emoji_progress == SXML_UNICODE_SEQUENCE_STARTED &&
               grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_ZWJ) {
        state->emoji_progress = SXML_UNICODE_SEQUENCE_READY;
    } else {
        state->emoji_progress = SXML_UNICODE_SEQUENCE_NONE;
    }
    if (properties.conjunct_break == SXML_UNICODE_CONJUNCT_BREAK_CONSONANT) {
        state->conjunct_progress = SXML_UNICODE_SEQUENCE_STARTED;
    } else if (state->conjunct_progress != SXML_UNICODE_SEQUENCE_NONE &&
               properties.conjunct_break == SXML_UNICODE_CONJUNCT_BREAK_LINKER) {
        state->conjunct_progress = SXML_UNICODE_SEQUENCE_READY;
    } else if (properties.conjunct_break != SXML_UNICODE_CONJUNCT_BREAK_EXTEND) {
        state->conjunct_progress = SXML_UNICODE_SEQUENCE_NONE;
    }
    state->previous_break = grapheme_break;
}

bool sxml_grapheme_cluster_iterator_next(sxml_grapheme_cluster_iterator *iterator,
                                         sxml_byte_range *cluster) {
    sxml_codepoint_iterator reader = {.string = iterator->string,
                                      .byte_offset = iterator->byte_offset};
    uint32_t codepoint = 0;
    /* GB2: no cluster after the end of text. */
    if (!sxml_codepoint_iterator_next(&reader, &codepoint)) {
        return false;
    }
    grapheme_state state = {.previous_break = SXML_UNICODE_GRAPHEME_BREAK_OTHER,
                            .regional_indicator_count = 0,
                            .emoji_progress = SXML_UNICODE_SEQUENCE_NONE,
                            .conjunct_progress = SXML_UNICODE_SEQUENCE_NONE};
    sxml_unicode_grapheme_properties properties;
    sxml_unicode_get_grapheme_properties(codepoint, &properties);
    advance_state(&state, properties);
    size_t end_offset = reader.byte_offset;
    while (sxml_codepoint_iterator_next(&reader, &codepoint)) {
        sxml_unicode_get_grapheme_properties(codepoint, &properties);
        if (is_boundary(&state, properties)) {
            break;
        }
        advance_state(&state, properties);
        end_offset = reader.byte_offset;
    }
    *cluster = (sxml_byte_range){.byte_offset = iterator->byte_offset,
                                 .byte_count = end_offset - iterator->byte_offset};
    iterator->byte_offset = end_offset;
    return true;
}

/* --------------------------------------------- Character reference names */

bool sxml_unicode_find_character_reference(sxml_byte_view name,
                                           sxml_unicode_character_reference_value *value) {
    size_t low_index = 0;
    size_t high_index = sxml_unicode_character_reference_count;
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const sxml_unicode_character_reference *middle =
            &sxml_unicode_character_references[middle_index];
        const size_t common_count =
            middle->name_byte_count < name.byte_count ? middle->name_byte_count : name.byte_count;
        int order = common_count == 0
                        ? 0
                        : memcmp(&sxml_unicode_reference_name_text[middle->name_offset],
                                 name.bytes, common_count);
        if (order == 0) {
            order = middle->name_byte_count < name.byte_count   ? -1
                    : middle->name_byte_count > name.byte_count ? 1
                                                                : 0;
        }
        if (order == 0) {
            const size_t second_number = middle->second_codepoint_number;
            value->codepoints[0] = middle->first_codepoint;
            value->codepoints[1] = 0;
            value->codepoint_count = 1;
            if (second_number != 0 &&
                second_number <= sxml_unicode_reference_second_codepoint_count) {
                value->codepoints[1] = sxml_unicode_reference_second_codepoints[second_number - 1];
                value->codepoint_count = 2;
            }
            return true;
        }
        if (order < 0) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return false;
}
