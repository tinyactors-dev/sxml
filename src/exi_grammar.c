/*
 * The built-in EXI grammars (EXI 1.0, 8.4; D333), their pruning by the
 * fidelity options (8.3), their learning, and event codes (6.2).
 *
 * Every nonterminal is learned productions, each with a one-part event
 * code (0 for the newest), followed by its fixed tail: the productions of
 * 8.4.1 to 8.4.3, numbered after the learned ones. The tails are computed
 * once per stream from the options.
 */
#include <stdalign.h>
#include <string.h>

#include "exi_internal.h"
#include "memory_internal.h"

/* A tail production before pruning: its terminal and full event code. */
typedef struct sxml_exi_tail_template {
    sxml_exi_event_kind event_kind;
    uint32_t parts[3];
    uint32_t part_count;
} sxml_exi_tail_template;

/* 8.4.1 Built-in Document Grammar. */
static const sxml_exi_tail_template sxml_exi_document_content[] = {
    {SXML_EXI_EVENT_START_ELEMENT_ANY, {0, 0, 0}, 1},
    {SXML_EXI_EVENT_DOCTYPE, {1, 0, 0}, 2},
    {SXML_EXI_EVENT_COMMENT, {1, 1, 0}, 3},
    {SXML_EXI_EVENT_PROCESSING_INSTRUCTION, {1, 1, 1}, 3},
};
static const sxml_exi_tail_template sxml_exi_document_end[] = {
    {SXML_EXI_EVENT_END_DOCUMENT, {0, 0, 0}, 1},
    {SXML_EXI_EVENT_COMMENT, {1, 0, 0}, 2},
    {SXML_EXI_EVENT_PROCESSING_INSTRUCTION, {1, 1, 0}, 2},
};
/* 8.4.2 Built-in Fragment Grammar. */
static const sxml_exi_tail_template sxml_exi_fragment_content[] = {
    {SXML_EXI_EVENT_START_ELEMENT_ANY, {0, 0, 0}, 1},
    {SXML_EXI_EVENT_END_DOCUMENT, {1, 0, 0}, 1},
    {SXML_EXI_EVENT_COMMENT, {2, 0, 0}, 2},
    {SXML_EXI_EVENT_PROCESSING_INSTRUCTION, {2, 1, 0}, 2},
};
/* 8.4.3 Built-in Element Grammar, with ChildContentItems expanded. */
static const sxml_exi_tail_template sxml_exi_start_tag_content[] = {
    {SXML_EXI_EVENT_END_ELEMENT, {0, 0, 0}, 2},
    {SXML_EXI_EVENT_ATTRIBUTE_ANY, {0, 1, 0}, 2},
    {SXML_EXI_EVENT_NAMESPACE, {0, 2, 0}, 2},
    {SXML_EXI_EVENT_SELF_CONTAINED, {0, 3, 0}, 2},
    {SXML_EXI_EVENT_START_ELEMENT_ANY, {0, 4, 0}, 2},
    {SXML_EXI_EVENT_CHARACTERS, {0, 5, 0}, 2},
    {SXML_EXI_EVENT_ENTITY_REFERENCE, {0, 6, 0}, 2},
    {SXML_EXI_EVENT_COMMENT, {0, 7, 0}, 3},
    {SXML_EXI_EVENT_PROCESSING_INSTRUCTION, {0, 7, 1}, 3},
};
static const sxml_exi_tail_template sxml_exi_element_content[] = {
    {SXML_EXI_EVENT_END_ELEMENT, {0, 0, 0}, 1},
    {SXML_EXI_EVENT_START_ELEMENT_ANY, {1, 0, 0}, 2},
    {SXML_EXI_EVENT_CHARACTERS, {1, 1, 0}, 2},
    {SXML_EXI_EVENT_ENTITY_REFERENCE, {1, 2, 0}, 2},
    {SXML_EXI_EVENT_COMMENT, {1, 3, 0}, 3},
    {SXML_EXI_EVENT_PROCESSING_INSTRUCTION, {1, 3, 1}, 3},
};

/* Whether the options keep productions of event_kind (8.3; the SC
   production only with selfContained, 8.4.3). */
static bool sxml_exi_is_kept(const sxml_exi_options *options, sxml_exi_event_kind event_kind) {
    switch (event_kind) {
    case SXML_EXI_EVENT_COMMENT:
        return options->fidelity.preserves_comments;
    case SXML_EXI_EVENT_PROCESSING_INSTRUCTION:
        return options->fidelity.preserves_processing_instructions;
    case SXML_EXI_EVENT_DOCTYPE:
    case SXML_EXI_EVENT_ENTITY_REFERENCE:
        return options->fidelity.preserves_dtd;
    case SXML_EXI_EVENT_NAMESPACE:
        return options->fidelity.preserves_prefixes;
    case SXML_EXI_EVENT_SELF_CONTAINED:
        return options->allows_self_contained;
    case SXML_EXI_EVENT_START_DOCUMENT:
    case SXML_EXI_EVENT_END_DOCUMENT:
    case SXML_EXI_EVENT_START_ELEMENT:
    case SXML_EXI_EVENT_START_ELEMENT_URI:
    case SXML_EXI_EVENT_START_ELEMENT_ANY:
    case SXML_EXI_EVENT_END_ELEMENT:
    case SXML_EXI_EVENT_ATTRIBUTE:
    case SXML_EXI_EVENT_ATTRIBUTE_URI:
    case SXML_EXI_EVENT_ATTRIBUTE_ANY:
    case SXML_EXI_EVENT_CHARACTERS:
        return true;
    }
    return true;
}

/* A template to prune: its productions and the options. */
typedef struct sxml_exi_pruning {
    const sxml_exi_tail_template *templates;
    size_t template_count;
    const sxml_exi_options *options;
} sxml_exi_pruning;

/*
 * Drops the productions the options do not keep and numbers the rest
 * again so every level is contiguous (8.3), keeping their order: a part
 * takes the next number when it or any part before it differs from the
 * previous kept production's.
 */
static void sxml_exi_prune(const sxml_exi_pruning *pruning, sxml_exi_tail *tail) {
    *tail = (sxml_exi_tail){0};
    uint32_t previous_original[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t current[3] = {0, 0, 0};
    bool has_previous = false;
    for (size_t template_index = 0; template_index < pruning->template_count; template_index++) {
        const sxml_exi_tail_template *template = &pruning->templates[template_index];
        if (!sxml_exi_is_kept(pruning->options, template->event_kind)) {
            continue;
        }
        uint32_t renumbered[3] = {0, 0, 0};
        bool differs = !has_previous;
        for (uint32_t part_index = 0; part_index < 3; part_index++) {
            if (part_index >= template->part_count) {
                break;
            }
            if (!differs && template->parts[part_index] != previous_original[part_index]) {
                differs = true;
                current[part_index]++;
                for (uint32_t later = part_index + 1; later < 3; later++) {
                    current[later] = 0;
                }
            } else if (differs && !has_previous) {
                current[part_index] = 0;
            }
            renumbered[part_index] = current[part_index];
        }
        for (uint32_t part_index = 0; part_index < 3; part_index++) {
            previous_original[part_index] = part_index < template->part_count ? template->parts[part_index] : UINT32_MAX;
        }
        has_previous = true;
        tail->productions[tail->production_count++] = (sxml_exi_tail_production){
            .event_kind = template->event_kind,
            .first_part = renumbered[0],
            .second_part = renumbered[1],
            .third_part = renumbered[2],
            .part_count = template->part_count,
        };
        if (renumbered[0] + 1 > tail->first_part_count) {
            tail->first_part_count = renumbered[0] + 1;
        }
    }
}

void sxml_exi_grammars_initialize(sxml_exi_grammars *grammars, const sxml_exi_grammars_configuration *configuration) {
    *grammars = (sxml_exi_grammars){
        .allocator = configuration->allocator,
        .maximum_learned_production_count = configuration->maximum_learned_production_count,
    };
    const struct {
        sxml_exi_nonterminal nonterminal;
        const sxml_exi_tail_template *templates;
        size_t template_count;
    } tails[] = {
        {SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT, sxml_exi_document_content,
         sizeof sxml_exi_document_content / sizeof sxml_exi_document_content[0]},
        {SXML_EXI_NONTERMINAL_DOCUMENT_END, sxml_exi_document_end,
         sizeof sxml_exi_document_end / sizeof sxml_exi_document_end[0]},
        {SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT, sxml_exi_fragment_content,
         sizeof sxml_exi_fragment_content / sizeof sxml_exi_fragment_content[0]},
        {SXML_EXI_NONTERMINAL_START_TAG_CONTENT, sxml_exi_start_tag_content,
         sizeof sxml_exi_start_tag_content / sizeof sxml_exi_start_tag_content[0]},
        {SXML_EXI_NONTERMINAL_ELEMENT_CONTENT, sxml_exi_element_content,
         sizeof sxml_exi_element_content / sizeof sxml_exi_element_content[0]},
    };
    for (size_t tail_index = 0; tail_index < sizeof tails / sizeof tails[0]; tail_index++) {
        sxml_exi_prune(&(sxml_exi_pruning){.templates = tails[tail_index].templates,
                                            .template_count = tails[tail_index].template_count,
                                            .options = configuration->options},
                       &grammars->tails[tails[tail_index].nonterminal]);
    }
}

void sxml_exi_grammars_finalize(sxml_exi_grammars *grammars) {
    for (size_t grammar_index = 0; grammar_index < grammars->element_grammar_count; grammar_index++) {
        sxml_allocator_release(grammars->allocator, grammars->element_grammars[grammar_index].start_tag_content.productions);
        sxml_allocator_release(grammars->allocator, grammars->element_grammars[grammar_index].element_content.productions);
    }
    sxml_allocator_release(grammars->allocator, grammars->element_grammars);
    sxml_allocator_release(grammars->allocator, grammars->fragment_content.productions);
    *grammars = (sxml_exi_grammars){.allocator = grammars->allocator};
}

sxml_status sxml_exi_grammars_add_element(sxml_exi_grammars *grammars, uint32_t *element_grammar) {
    if (grammars->element_grammar_count == grammars->element_grammar_capacity) {
        void *elements = grammars->element_grammars;
        sxml_status status = sxml_allocator_reserve_array(
            grammars->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                   .capacity = &grammars->element_grammar_capacity,
                                                                   .element_count = grammars->element_grammar_count,
                                                                   .required_count = grammars->element_grammar_count + 1,
                                                                   .element_size = sizeof(sxml_exi_element_grammar),
                                                                   .element_alignment = alignof(sxml_exi_element_grammar),
                                                                   .minimum_capacity = 16});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        grammars->element_grammars = elements;
    }
    grammars->element_grammars[grammars->element_grammar_count] = (sxml_exi_element_grammar){0};
    *element_grammar = (uint32_t)grammars->element_grammar_count;
    grammars->element_grammar_count++;
    return SXML_STATUS_OK;
}

sxml_exi_learned *sxml_exi_grammars_learned(sxml_exi_grammars *grammars, sxml_exi_position position) {
    switch (position.nonterminal) {
    case SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT:
        return &grammars->fragment_content;
    case SXML_EXI_NONTERMINAL_START_TAG_CONTENT:
        return position.element_grammar < grammars->element_grammar_count
                   ? &grammars->element_grammars[position.element_grammar].start_tag_content
                   : NULL;
    case SXML_EXI_NONTERMINAL_ELEMENT_CONTENT:
        return position.element_grammar < grammars->element_grammar_count
                   ? &grammars->element_grammars[position.element_grammar].element_content
                   : NULL;
    case SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT:
    case SXML_EXI_NONTERMINAL_DOCUMENT_END:
    case SXML_EXI_NONTERMINAL_COUNT:
        return NULL;
    }
    return NULL;
}

sxml_status sxml_exi_grammars_learn(sxml_exi_grammars *grammars, const sxml_exi_learning *learning) {
    sxml_exi_learned *learned = sxml_exi_grammars_learned(grammars, learning->position);
    if (learned == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (grammars->learned_production_count >= grammars->maximum_learned_production_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (learned->production_count == learned->production_capacity) {
        void *elements = learned->productions;
        sxml_status status = sxml_allocator_reserve_array(
            grammars->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                   .capacity = &learned->production_capacity,
                                                                   .element_count = learned->production_count,
                                                                   .required_count = learned->production_count + 1,
                                                                   .element_size = sizeof(sxml_exi_production),
                                                                   .element_alignment = alignof(sxml_exi_production),
                                                                   .minimum_capacity = 4});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        learned->productions = elements;
    }
    /* Event code 0 is the newest: the others' first part goes up by one. */
    memmove(learned->productions + 1, learned->productions, learned->production_count * sizeof *learned->productions);
    learned->productions[0] = learning->production;
    learned->production_count++;
    grammars->learned_production_count++;
    return SXML_STATUS_OK;
}

/* The counts at each level that size an event code's parts (6.2). */
typedef struct sxml_exi_part_counts {
    uint32_t first_count;
    uint32_t second_count;
    uint32_t third_count;
} sxml_exi_part_counts;

static sxml_exi_part_counts sxml_exi_count_parts(const sxml_exi_tail *tail, const sxml_exi_tail_production *production) {
    sxml_exi_part_counts counts = {0};
    for (size_t production_index = 0; production_index < tail->production_count; production_index++) {
        const sxml_exi_tail_production *sibling = &tail->productions[production_index];
        if (sibling->first_part != production->first_part) {
            continue;
        }
        if (sibling->part_count >= 2 && sibling->second_part + 1 > counts.second_count) {
            counts.second_count = sibling->second_part + 1;
        }
        if (sibling->part_count >= 3 && sibling->second_part == production->second_part &&
            sibling->third_part + 1 > counts.third_count) {
            counts.third_count = sibling->third_part + 1;
        }
    }
    return counts;
}

static uint32_t sxml_exi_learned_count(sxml_exi_grammars *grammars, sxml_exi_position position) {
    const sxml_exi_learned *learned = sxml_exi_grammars_learned(grammars, position);
    return learned == NULL ? 0U : (uint32_t)learned->production_count;
}

sxml_status sxml_exi_write_event_code(sxml_exi_grammars *grammars, const sxml_exi_event_code_request *request,
                                      sxml_exi_bit_writer *writer) {
    const sxml_exi_tail *tail = &grammars->tails[request->position.nonterminal];
    const uint32_t learned_count = sxml_exi_learned_count(grammars, request->position);
    const uint32_t first_width = sxml_exi_bit_width((uint64_t)learned_count + tail->first_part_count);
    if (request->match.is_learned) {
        return sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = request->match.index, .bit_count = first_width});
    }
    const sxml_exi_tail_production *production = &tail->productions[request->match.index];
    sxml_status status =
        sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = learned_count + production->first_part, .bit_count = first_width});
    const sxml_exi_part_counts counts = sxml_exi_count_parts(tail, production);
    if (status == SXML_STATUS_OK && production->part_count >= 2) {
        status = sxml_exi_write_n_bit(
            writer, (sxml_exi_bits){.value = production->second_part, .bit_count = sxml_exi_bit_width(counts.second_count)});
    }
    if (status == SXML_STATUS_OK && production->part_count >= 3) {
        status = sxml_exi_write_n_bit(
            writer, (sxml_exi_bits){.value = production->third_part, .bit_count = sxml_exi_bit_width(counts.third_count)});
    }
    return status;
}

sxml_status sxml_exi_read_event_code(sxml_exi_grammars *grammars, const sxml_exi_event_code_read *read,
                                     sxml_exi_match *match) {
    const sxml_exi_tail *tail = &grammars->tails[read->position.nonterminal];
    const uint32_t learned_count = sxml_exi_learned_count(grammars, read->position);
    uint32_t first = 0;
    sxml_status status = sxml_exi_read_n_bit(
        read->reader,
        (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width((uint64_t)learned_count + tail->first_part_count)},
        &first);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (first < learned_count) {
        *match = (sxml_exi_match){.is_learned = true, .index = first};
        return SXML_STATUS_OK;
    }
    const uint32_t tail_first = first - learned_count;
    uint32_t second = 0;
    uint32_t third = 0;
    bool has_second = false;
    bool has_third = false;
    for (size_t production_index = 0; production_index < tail->production_count; production_index++) {
        const sxml_exi_tail_production *production = &tail->productions[production_index];
        if (production->first_part != tail_first) {
            continue;
        }
        if (production->part_count >= 2 && !has_second) {
            const sxml_exi_part_counts counts = sxml_exi_count_parts(tail, production);
            status = sxml_exi_read_n_bit(
                read->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(counts.second_count)}, &second);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            has_second = true;
        }
        if (production->part_count >= 2 && production->second_part != second) {
            continue;
        }
        if (production->part_count >= 3 && !has_third) {
            const sxml_exi_part_counts counts = sxml_exi_count_parts(tail, production);
            status = sxml_exi_read_n_bit(
                read->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(counts.third_count)}, &third);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            has_third = true;
        }
        if (production->part_count >= 3 && production->third_part != third) {
            continue;
        }
        *match = (sxml_exi_match){.is_learned = false, .index = (uint32_t)production_index};
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_MALFORMED_INPUT;
}

sxml_exi_production sxml_exi_match_production(sxml_exi_grammars *grammars, sxml_exi_position position,
                                              sxml_exi_match match) {
    if (match.is_learned) {
        const sxml_exi_learned *learned = sxml_exi_grammars_learned(grammars, position);
        return learned->productions[match.index];
    }
    return (sxml_exi_production){.event_kind = grammars->tails[position.nonterminal].productions[match.index].event_kind,
                                 .uri = SXML_EXI_NONE,
                                 .local_name = SXML_EXI_NONE};
}
