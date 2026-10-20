/*
 * fn:invisible-xml (F&O 4.0, 17.4; Invisible XML Specification 1.0): the
 * Invisible XML feature's library function (src/ixml_internal.h, D191,
 * D215). Not a library part of the registry: its row view is
 * sxml_ixml_library_functions, which the processor serves only when its
 * configuration names sxml_invisible_xml_feature(); without the feature
 * the function is absent (XPST0017).
 *
 * fn:invisible-xml compiles the grammar once and returns a parser
 * function whose body holds the compiled grammar (reference-counted
 * through the body's retain and release). A grammar in ixml notation is
 * parsed with the grammar of grammars, and its parse is sent to the
 * grammar builder; a grammar in XML form is copied to it; no grammar is
 * the grammar of grammars itself.
 *
 * Golden file: src/function_library_schema.c (fn:xsd-validator: options,
 * then a function item whose body does the work).
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "ixml_internal.h"
#include "memory_internal.h"
#include "processor_internal.h"
#include "xdm_internal.h"

#include <string.h>

/* The options of fn:invisible-xml. */
enum {
    SXML_IXML_OPTION_FAIL_ON_ERROR,
    SXML_IXML_OPTION_COUNT,
};

static const sxml_option_definition ixml_options[SXML_IXML_OPTION_COUNT] = {
    SXML_FUNCTION_OPTION("fail-on-error", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
};

static const sxml_static_error grammar_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The grammar is not a conforming Invisible XML grammar: it does not match the ixml "
        "grammar, or its XML form is not the form of one (F&O 4.0, fn:invisible-xml)"),
};

/* The static errors of the Specification ("Errors"), by
   sxml_ixml_grammar_error. */
static const sxml_static_error grammar_errors[] = {
    [SXML_IXML_GRAMMAR_ERROR_UNDEFINED_NONTERMINAL] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0001"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML S02: the grammar uses a nonterminal that no rule defines")},
    [SXML_IXML_GRAMMAR_ERROR_DUPLICATE_RULE] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0001"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML S03: the grammar has more than one rule for a nonterminal")},
    [SXML_IXML_GRAMMAR_ERROR_CODEPOINT_RANGE] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0001"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML S07: a hexadecimal character is beyond the Unicode code-point range")},
    [SXML_IXML_GRAMMAR_ERROR_NONCHARACTER] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0001"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML S08: a hexadecimal character is a noncharacter or a surrogate")},
    [SXML_IXML_GRAMMAR_ERROR_RANGE_ORDER] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0001"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML S09: the first character of a range is after its last")},
    [SXML_IXML_GRAMMAR_ERROR_UNKNOWN_CLASS] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0001"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML S10: a character class is not a Unicode general category")},
};

static const sxml_static_error input_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The input does not match the grammar, and fail-on-error is true (F&O 4.0, "
        "fn:invisible-xml)"),
};

/* The dynamic errors of the Specification ("Errors"), by
   sxml_ixml_construction_error: the parse has no XML, so the parser
   function raises FOIX0002 whatever fail-on-error says (D215). */
static const sxml_static_error construction_errors[] = {
    [SXML_IXML_CONSTRUCTION_ERROR_DUPLICATE_ATTRIBUTE] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0002"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML D02: the parse puts two attributes of one name on an element")},
    [SXML_IXML_CONSTRUCTION_ERROR_NAME] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0002"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML D03: the parse names an element or attribute with a name that is not an XML "
             "name")},
    [SXML_IXML_CONSTRUCTION_ERROR_CHARACTER] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0002"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML D04: the parse outputs a character that XML does not allow")},
    [SXML_IXML_CONSTRUCTION_ERROR_ROOT_ATTRIBUTE] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0002"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML D05: the parse makes an attribute the root of the document")},
    [SXML_IXML_CONSTRUCTION_ERROR_ROOT_ELEMENT] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0002"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML D06: the parse does not make exactly one element, and no text, at the top of "
             "the document")},
    [SXML_IXML_CONSTRUCTION_ERROR_XMLNS_ATTRIBUTE] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOIX0002"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "iXML D07: the parse makes an attribute named xmlns")},
};

/* ------------------------------------------------- Error descriptions */

/* Error descriptions that name what is wrong and where (D283), built in
   item-heap memory and raised through the heap, which keeps the text. */
typedef struct described_error {
    const sxml_static_error *error;
    sxml_byte_view description;
} described_error;

static sxml_status report_described(const sxml_function_call *call, described_error error) {
    return sxml_item_heap_raise_error(
        call->item_heap,
        &(sxml_error_raise_request){
            .error_status = error.error->error_status,
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = error.error->code_local_name,
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .description = error.description,
            .has_value = false,
            .value = sxml_sequence_empty(),
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}},
        call->error_record);
}

/* The pieces of a description: text and names, in order. */
typedef struct description_pieces {
    const sxml_byte_view *pieces;
    size_t piece_count;
} description_pieces;

static sxml_status report_pieces(const sxml_function_call *call, const sxml_static_error *error,
                                 description_pieces pieces) {
    sxml_xml_buffer text = sxml_xml_buffer_empty(sxml_item_heap_as_allocator(call->item_heap));
    sxml_status status = SXML_STATUS_OK;
    for (size_t piece_index = 0; status == SXML_STATUS_OK && piece_index < pieces.piece_count;
         piece_index += 1) {
        status = sxml_xml_buffer_append(&text, pieces.pieces[piece_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = report_described(
            call, (described_error){.error = error,
                                    .description = {.bytes = text.bytes,
                                                    .byte_count = text.byte_count}});
    }
    sxml_xml_buffer_release(&text);
    return status;
}

static sxml_status append_number(sxml_xml_buffer *text, size_t number) {
    uint8_t digits[24];
    size_t digit_count = 0;
    size_t rest = number;
    do {
        digits[digit_count] = (uint8_t)('0' + rest % 10);
        digit_count += 1;
        rest /= 10;
    } while (rest != 0);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && digit_count > 0) {
        digit_count -= 1;
        status = sxml_xml_buffer_append_byte(text, digits[digit_count]);
    }
    return status;
}

/* FOIX0001 for grammar text that does not match the ixml grammar: where
   reading it stopped, as in a failure document. */
static sxml_status report_syntax_error(const sxml_function_call *call,
                                       sxml_ixml_failure_location location) {
    sxml_xml_buffer place = sxml_xml_buffer_empty(sxml_item_heap_as_allocator(call->item_heap));
    sxml_status status = sxml_xml_buffer_append(&place, SXML_BYTE_VIEW_FROM_LITERAL("line "));
    if (status == SXML_STATUS_OK) {
        status = append_number(&place, location.line_number);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&place, SXML_BYTE_VIEW_FROM_LITERAL(", column "));
    }
    if (status == SXML_STATUS_OK) {
        status = append_number(&place, location.column_number);
    }
    if (status == SXML_STATUS_OK && location.codepoint == SXML_IXML_NONE) {
        status = sxml_xml_buffer_append(&place,
                                        SXML_BYTE_VIEW_FROM_LITERAL(", at the end of the grammar"));
    } else if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&place, SXML_BYTE_VIEW_FROM_LITERAL(", at "));
        if (status == SXML_STATUS_OK) {
            status = sxml_ixml_append_character_notation(&place, location.codepoint);
        }
    }
    if (status == SXML_STATUS_OK) {
        const sxml_byte_view pieces[] = {
            SXML_BYTE_VIEW_FROM_LITERAL("The grammar does not match the ixml grammar at "),
            {.bytes = place.bytes, .byte_count = place.byte_count},
            SXML_BYTE_VIEW_FROM_LITERAL(" (F&O 4.0, fn:invisible-xml)")};
        status = report_pieces(call, &grammar_error,
                               (description_pieces){.pieces = pieces, .piece_count = 3});
    }
    sxml_xml_buffer_release(&place);
    return status;
}

/* FOIX0001 for a grammar that does not conform, naming the rule when the
   error is about one (S02, S03). */
static sxml_status report_grammar_error(const sxml_function_call *call,
                                        const sxml_ixml_grammar_outcome *outcome) {
    if (outcome->error == SXML_IXML_GRAMMAR_ERROR_UNDEFINED_NONTERMINAL) {
        const sxml_byte_view pieces[] = {
            SXML_BYTE_VIEW_FROM_LITERAL("iXML S02: the grammar uses the nonterminal "), outcome->name,
            SXML_BYTE_VIEW_FROM_LITERAL(", which no rule defines")};
        return report_pieces(call, &grammar_errors[outcome->error],
                             (description_pieces){.pieces = pieces, .piece_count = 3});
    }
    if (outcome->error == SXML_IXML_GRAMMAR_ERROR_DUPLICATE_RULE) {
        const sxml_byte_view pieces[] = {
            SXML_BYTE_VIEW_FROM_LITERAL("iXML S03: the grammar has more than one rule for "),
            outcome->name};
        return report_pieces(call, &grammar_errors[outcome->error],
                             (description_pieces){.pieces = pieces, .piece_count = 2});
    }
    return sxml_function_library_report(call, outcome->error == SXML_IXML_GRAMMAR_ERROR_FORM
                                                  ? &grammar_error
                                                  : &grammar_errors[outcome->error]);
}

/* FOIX0002 for a parse that cannot be XML, naming the names it is about
   (D02, D03). */
static sxml_status report_construction_error(const sxml_function_call *call,
                                             const sxml_ixml_construction_outcome *outcome) {
    const sxml_static_error *error = &construction_errors[outcome->error];
    if (outcome->error == SXML_IXML_CONSTRUCTION_ERROR_DUPLICATE_ATTRIBUTE) {
        const sxml_byte_view pieces[] = {
            SXML_BYTE_VIEW_FROM_LITERAL("iXML D02: the parse puts two attributes named "),
            outcome->name, SXML_BYTE_VIEW_FROM_LITERAL(" on the element "), outcome->element_name};
        return report_pieces(call, error, (description_pieces){.pieces = pieces, .piece_count = 4});
    }
    if (outcome->error == SXML_IXML_CONSTRUCTION_ERROR_NAME) {
        const sxml_byte_view pieces[] = {
            SXML_BYTE_VIEW_FROM_LITERAL("iXML D03: the parse names an element or attribute "),
            outcome->name, SXML_BYTE_VIEW_FROM_LITERAL(", which is not an XML name")};
        return report_pieces(call, error, (description_pieces){.pieces = pieces, .piece_count = 3});
    }
    return sxml_function_library_report(call, error);
}

/* ----------------------------------------------------------- Input */

/* The codepoints of UTF-8 text, line ends normalized and a leading byte
   order mark dropped (the Specification, "Parsing"), in allocator
   memory. */
typedef struct codepoint_input {
    sxml_allocator allocator;
    uint32_t *codepoints;
    size_t codepoint_count;
} codepoint_input;

static sxml_status decode_input(sxml_byte_view utf8, codepoint_input *input) {
    input->codepoints = NULL;
    input->codepoint_count = 0;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        input->allocator,
        (sxml_array_allocation_request){.element_count = utf8.byte_count,
                                        .element_size = sizeof(uint32_t),
                                        .element_alignment = _Alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    input->codepoints = memory;
    for (size_t byte_offset = 0; byte_offset < utf8.byte_count;) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(utf8, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        uint32_t codepoint = decoded.codepoint;
        if (codepoint == 0xFEFFU && input->codepoint_count == 0 &&
            byte_offset == decoded.encoded_byte_count) {
            continue;
        }
        if (codepoint == '\r') {
            /* CR LF and a lone CR are one LF (XML 1.0, 2.11). */
            codepoint = '\n';
            if (byte_offset < utf8.byte_count && utf8.bytes[byte_offset] == '\n') {
                byte_offset += 1;
            }
        }
        input->codepoints[input->codepoint_count] = codepoint;
        input->codepoint_count += 1;
    }
    return SXML_STATUS_OK;
}

static void release_input(codepoint_input *input) {
    sxml_allocator_release(input->allocator, input->codepoints);
    input->codepoints = NULL;
    input->codepoint_count = 0;
}

static sxml_ixml_codepoint_view input_view(const codepoint_input *input) {
    return (sxml_ixml_codepoint_view){.codepoints = input->codepoints,
                                      .codepoint_count = input->codepoint_count};
}

/* Charges the parse's steps to the call (the evaluation's step limit). */
static sxml_status charge_call_steps(void *state, uint64_t step_count) {
    return sxml_function_library_charge_steps(state, step_count);
}

static const sxml_ixml_step_meter_operations call_meter_operations = {
    .charge = charge_call_steps,
};

static sxml_ixml_step_meter call_meter(const sxml_function_call *call) {
    return (sxml_ixml_step_meter){.state = (void *)call, .operations = &call_meter_operations};
}

/* -------------------------------------------------------- Grammars */

/* What reading a grammar gave: whether it is a grammar, and why not. */
typedef struct grammar_answer {
    bool is_grammar;
    sxml_ixml_grammar_error error;
    /* Grammar text that does not match the ixml grammar: where reading
       it stopped. */
    bool has_location;
    sxml_ixml_failure_location location;
} grammar_answer;

/* Compiles the grammar of grammars into grammar (zeroed). */
static sxml_status compile_grammar_of_grammars(sxml_allocator allocator,
                                               sxml_ixml_grammar *grammar) {
    sxml_ixml_grammar_builder builder;
    sxml_ixml_grammar_builder_initialize(
        &builder, &(sxml_ixml_grammar_builder_configuration){.allocator = allocator,
                                                             .name_pool = NULL});
    sxml_status status =
        sxml_ixml_grammar_builder_read_entries(&builder, sxml_ixml_grammar_of_grammars);
    sxml_ixml_grammar_outcome outcome = {.error = SXML_IXML_GRAMMAR_ERROR_NONE,
                                         .name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_ixml_grammar_builder_finish(&builder, grammar, &outcome);
    }
    sxml_ixml_grammar_builder_finalize(&builder);
    if (status == SXML_STATUS_OK && outcome.error != SXML_IXML_GRAMMAR_ERROR_NONE) {
        /* The table is generated from a conforming grammar. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    return status;
}

/* Where a grammar is read: the call, the processor and the builder. */
typedef struct grammar_source {
    const sxml_function_call *call;
    sxml_processor *processor;
    sxml_ixml_grammar_builder *builder;
} grammar_source;

/* Parses grammar text in ixml notation with the grammar of grammars and
   sends its XML form to the source's builder. */
static sxml_status read_grammar_text(const grammar_source *source, sxml_byte_view text,
                                     grammar_answer *answer) {
    const sxml_allocator allocator = source->builder->allocator;
    sxml_ixml_grammar grammar_of_grammars;
    sxml_status status = compile_grammar_of_grammars(allocator, &grammar_of_grammars);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    codepoint_input input = {.allocator = allocator, .codepoints = NULL, .codepoint_count = 0};
    status = decode_input(text, &input);
    sxml_ixml_parse_result parse = sxml_ixml_parse_result_empty(allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_ixml_parse(&(sxml_ixml_parse_request){.grammar = &grammar_of_grammars,
                                                            .input = input_view(&input),
                                                            .step_meter = call_meter(source->call)},
                                 &parse);
    }
    if (status == SXML_STATUS_OK && !parse.is_sentence) {
        answer->is_grammar = false;
        answer->has_location = true;
        answer->location = sxml_ixml_locate_failure(
            (sxml_ixml_failure_request){.parse = &parse, .input = input_view(&input)});
    } else if (status == SXML_STATUS_OK) {
        sxml_ixml_construction_outcome outcome = {
            .error = SXML_IXML_CONSTRUCTION_ERROR_NONE,
            .name = {.bytes = NULL, .byte_count = 0},
            .element_name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_ixml_construct(
            &(sxml_ixml_construction_request){
                .grammar = &grammar_of_grammars,
                .parse = &parse,
                .input = input_view(&input),
                .builder = sxml_ixml_grammar_builder_as_tree_builder(source->builder),
                .name_pool = source->processor->name_pool,
                .allocator = allocator},
            &outcome);
        if (outcome.error != SXML_IXML_CONSTRUCTION_ERROR_NONE) {
            answer->is_grammar = false;
        }
    }
    sxml_ixml_parse_result_finalize(&parse);
    release_input(&input);
    sxml_ixml_grammar_finalize(&grammar_of_grammars);
    return status;
}

/* True when node is an element named ixml in no namespace. */
static bool is_ixml_element(const sxml_processor *processor, sxml_node_reference node) {
    sxml_node_description description;
    if (sxml_node_describe(node, &description) != SXML_STATUS_OK ||
        description.node_kind != SXML_NODE_KIND_ELEMENT) {
        return false;
    }
    sxml_name_parts parts;
    return sxml_name_pool_get_name_parts(processor->name_pool, description.node_name, &parts) ==
               SXML_STATUS_OK &&
           parts.namespace_uri.index == SXML_NAMESPACE_NONE &&
           sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("ixml"));
}

/* Reads the $grammar argument into the source's builder: the grammar of
   grammars for the empty sequence, a string in ixml notation, or an ixml
   element in XML form. */
static sxml_status read_grammar_argument(const grammar_source *source, grammar_answer *answer) {
    sxml_optional_item grammar = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(source->call, 0, &grammar);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!grammar.has_item) {
        return sxml_ixml_grammar_builder_read_entries(source->builder,
                                                      sxml_ixml_grammar_of_grammars);
    }
    if (sxml_item_get_kind(grammar.item) == SXML_ITEM_KIND_NODE) {
        sxml_node_reference node;
        status = sxml_item_get_node(grammar.item, &node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!is_ixml_element(source->processor, node)) {
            answer->is_grammar = false;
            return SXML_STATUS_OK;
        }
        return sxml_tree_builder_copy_node(
            sxml_ixml_grammar_builder_as_tree_builder(source->builder),
            &(sxml_copy_request){.node = node,
                                 .preserves_namespaces = false,
                                 .inherits_namespaces = false,
                                 .preserves_type_annotations = false,
                                 .strips_identity = false});
    }
    sxml_string text;
    status = sxml_function_library_read_string_argument(source->call, 0, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return read_grammar_text(source, text.utf8, answer);
}

/* ------------------------------------------------- Parser functions */

/* The state of a parser function's body: the compiled grammar and the
   option, shared by the function item and its copies. */
typedef struct ixml_parser {
    sxml_allocator allocator;
    size_t reference_count;
    bool fails_on_error;
    sxml_ixml_grammar grammar;
} ixml_parser;

static void retain_parser(void *state) {
    ixml_parser *parser = state;
    parser->reference_count += 1;
}

static void release_parser(void *state) {
    ixml_parser *parser = state;
    parser->reference_count -= 1;
    if (parser->reference_count == 0) {
        sxml_ixml_grammar_finalize(&parser->grammar);
        sxml_allocator_release(parser->allocator, parser);
    }
}

/* Builds the document of a parse in an arena tree of processor. */
typedef struct document_request {
    sxml_processor *processor;
    const ixml_parser *parser;
    const sxml_ixml_parse_result *parse;
    sxml_ixml_codepoint_view input;
} document_request;

static sxml_status build_document(const document_request *request,
                                  sxml_ixml_construction_outcome *outcome,
                                  sxml_item *document_node) {
    sxml_processor *processor = request->processor;
    const sxml_allocator allocator = processor->allocator;
    sxml_arena_tree *arena_tree = NULL;
    /* The tree destroys itself with its last reference (D39). */
    sxml_status status = sxml_arena_tree_create(
        allocator,
        &(sxml_arena_tree_configuration){
            .allocator = allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = processor->item_heap,
            .maximum_node_count = processor->limits.maximum_tree_node_count,
            .maximum_depth = processor->limits.xml_limits.maximum_depth,
            .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
            .keeps_locations = false,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL,
            .base_uri = {.bytes = NULL, .byte_count = 0}},
        &arena_tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_ixml_construct(
            &(sxml_ixml_construction_request){
                .grammar = &request->parser->grammar,
                .parse = request->parse,
                .input = request->input,
                .builder = sxml_arena_tree_as_tree_builder(arena_tree),
                .name_pool = processor->name_pool,
                .allocator = allocator},
            outcome);
    }
    sxml_tree *finished = NULL;
    if (status == SXML_STATUS_OK && outcome->error == SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        status = sxml_arena_tree_finish(arena_tree, &finished);
    }
    if (status != SXML_STATUS_OK || outcome->error != SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document_node = sxml_node_item_make((sxml_node_reference){
        .tree = finished, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(finished);
    return SXML_STATUS_OK;
}

/* The parser function P($input as xs:string) as item(): the document of
   the parse of $input, or of its failure (F&O 4.0, fn:invisible-xml). */
static sxml_status call_parser(void *state, const sxml_function_call *call,
                               sxml_sequence *result) {
    const ixml_parser *parser = state;
    const sxml_evaluation_environment *environment =
        call->dynamic_context == NULL
            ? NULL
            : sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_string text;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    codepoint_input input = {.allocator = allocator, .codepoints = NULL, .codepoint_count = 0};
    status = decode_input(text.utf8, &input);
    sxml_ixml_parse_result parse = sxml_ixml_parse_result_empty(allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_ixml_parse(&(sxml_ixml_parse_request){.grammar = &parser->grammar,
                                                            .input = input_view(&input),
                                                            .step_meter = call_meter(call)},
                                 &parse);
    }
    sxml_ixml_construction_outcome outcome = {.error = SXML_IXML_CONSTRUCTION_ERROR_NONE,
                                              .name = {.bytes = NULL, .byte_count = 0},
                                              .element_name = {.bytes = NULL, .byte_count = 0}};
    sxml_item document_node = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (status == SXML_STATUS_OK && !parse.is_sentence && parser->fails_on_error) {
        status = sxml_function_library_report(call, &input_error);
    } else if (status == SXML_STATUS_OK) {
        status = build_document(&(document_request){.processor = environment->processor,
                                                    .parser = parser,
                                                    .parse = &parse,
                                                    .input = input_view(&input)},
                                &outcome, &document_node);
    }
    sxml_ixml_parse_result_finalize(&parse);
    release_input(&input);
    if (status == SXML_STATUS_OK && outcome.error != SXML_IXML_CONSTRUCTION_ERROR_NONE) {
        return report_construction_error(call, &outcome);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(document_node);
    }
    return status;
}

static const sxml_function_body_operations parser_body_operations = {
    .call = call_parser,
    .retain = retain_parser,
    .release = release_parser,
};

/* The signature of P: fn(xs:string) as item(). */
static sxml_status parser_signature(const sxml_function_call *call,
                                    sxml_type_identifier *signature) {
    const sxml_sequence_type parameter = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    return sxml_type_registry_register_item_type(
        sxml_item_heap_get_type_registry(call->item_heap),
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types = {.sequence_types = &parameter,
                                                                      .sequence_type_count = 1},
                                                  .result_type = {.item_type =
                                                                      sxml_builtin_type_identifier(
                                                                          SXML_TYPE_ITEM),
                                                                  .occurrence =
                                                                      SXML_OCCURRENCE_EXACTLY_ONE}}}},
        signature);
}

/* Makes the parser function of parser, owned, in *result. */
static sxml_status make_parser_function(const sxml_function_call *call, ixml_parser *parser,
                                        sxml_sequence *result) {
    sxml_type_identifier signature = {.index = SXML_TYPE_NONE};
    sxml_status status = parser_signature(call, &signature);
    sxml_item function_item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_item_make(
            call->item_heap,
            &(sxml_function_item_request){
                .function_name = {.index = 0},
                .signature = signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = NULL, .sequence_count = 0},
                .body = {.state = parser, .operations = &parser_body_operations}},
            &function_item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(function_item);
    }
    return status;
}

/* fn:invisible-xml($grammar as (xs:string | element(Q{}ixml))? := (),
   $options as map(*)? := {}) as fn(xs:string) as item() (F&O 4.0,
   17.4): FOIX0001 for a grammar that is not a conforming iXML grammar. */
static sxml_status call_invisible_xml(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_evaluation_environment *environment =
        call->dynamic_context == NULL
            ? NULL
            : sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_option_values values = {.is_present = {false}};
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 1,
                               .definitions = {ixml_options, SXML_IXML_OPTION_COUNT}},
        &values);
    const bool fails_on_error = sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = &values, .option_index = SXML_IXML_OPTION_FAIL_ON_ERROR});
    sxml_function_library_release_options(&values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = sizeof(ixml_parser),
                                  .alignment = _Alignof(ixml_parser)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ixml_parser *parser = memory;
    memset(parser, 0, sizeof(*parser));
    parser->allocator = allocator;
    parser->reference_count = 1;
    parser->fails_on_error = fails_on_error;
    sxml_ixml_grammar_builder builder;
    sxml_ixml_grammar_builder_initialize(
        &builder, &(sxml_ixml_grammar_builder_configuration){
                      .allocator = allocator, .name_pool = environment->processor->name_pool});
    grammar_answer answer = {.is_grammar = true,
                             .error = SXML_IXML_GRAMMAR_ERROR_NONE,
                             .has_location = false,
                             .location = {.line_number = 0, .column_number = 0, .codepoint = 0}};
    status = read_grammar_argument(
        &(grammar_source){.call = call, .processor = environment->processor, .builder = &builder},
        &answer);
    sxml_ixml_grammar_outcome outcome = {.error = SXML_IXML_GRAMMAR_ERROR_NONE,
                                         .name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK && answer.is_grammar) {
        status = sxml_ixml_grammar_builder_finish(&builder, &parser->grammar, &outcome);
    }
    /* The outcome's name is the builder's, so the errors are raised
       before it is finalized. */
    if (status == SXML_STATUS_OK && !answer.is_grammar && answer.has_location) {
        status = report_syntax_error(call, answer.location);
    } else if (status == SXML_STATUS_OK && !answer.is_grammar) {
        status = sxml_function_library_report(call, &grammar_error);
    } else if (status == SXML_STATUS_OK && outcome.error != SXML_IXML_GRAMMAR_ERROR_NONE) {
        status = report_grammar_error(call, &outcome);
    }
    sxml_ixml_grammar_builder_finalize(&builder);
    if (status == SXML_STATUS_OK) {
        status = make_parser_function(call, parser, result);
    }
    /* The function item holds its own reference. */
    release_parser(parser);
    return status;
}

static const sxml_function_parameter invisible_xml_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("grammar"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("(xs:string | element(Q{}ixml))?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY,
     .default_function_number = 0},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("options"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("map(*)?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY,
     .default_function_number = 0},
};

static const sxml_native_function ixml_functions[] = {
    SXML_FUNCTION_ROW("invisible-xml", invisible_xml_parameters, "fn(xs:string) as item()",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_invisible_xml),
};

const sxml_native_function_view sxml_ixml_library_functions = {
    .native_functions = ixml_functions,
    .native_function_count = sizeof(ixml_functions) / sizeof(ixml_functions[0]),
};
