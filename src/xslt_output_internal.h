#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xslt.h"
#include "xslt_ast_internal.h"

/*
 * Output and result documents (XSLT 4.0, section 26; docs/DECISIONS.md
 * D112, D146, D156; packet 4.13). Internal to sxml.
 *
 * Two objects, one compiled and one per transformation:
 *
 * - The stylesheet outputs (glossary `stylesheet_outputs`), made once from
 *   the sealed stylesheet AST: every output definition (26.1.1: the
 *   xsl:output declarations of one name merged by import precedence,
 *   XTSE1560), every named character map (26.1.6: XTSE1580, XTSE1590,
 *   XTSE1600) and every result document instruction (glossary
 *   `result_document_instruction`). The attributes of xsl:output become
 *   serialization parameter texts (D146), QNames as EQNames, and are read
 *   by sxml_serialization_parameter_reader_read_texts with the parameter
 *   document over them (26.1.5: the document wins), the same reader as
 *   XQuery's output declarations; build-tree is read here (it is no
 *   serialization parameter) and use-character-maps from the named
 *   character maps. The stylesheet outputs keep nothing of the AST.
 * - The result writer (glossary `result_writer`) of one transformation:
 *   it reaches the rows below through the evaluation environment
 *   (src/processor_internal.h), delivers each result document to the
 *   result document handler of include/sxml/xslt.h, keeps the URIs
 *   written (XTDE1490) and the current output URI (26.3), and delivers the
 *   principal result.
 *
 * xsl:result-document (26.2) is lowered (sxml_xslt_instruction_rules_output)
 * into two calls of internal rows around its content:
 *
 *     let $open := result-document-open(N, (V1, V2, ...))
 *     let $raw := <select or sequence constructor>
 *     return result-document-close($open, $raw)
 *
 * N is the instruction's AST node index, which names its result document
 * instruction among the stylesheet outputs, and V1... are the values of
 * its attribute value templates in the fixed order of the instruction
 * (format, href, then the serialization attributes). Open resolves href
 * against the base output URI, checks XTDE1490 and merges the definition
 * (26.2.4), then makes the URI the current output URI; the content is
 * evaluated after it (a LET is evaluated before its body), so
 * fn:current-output-uri in it, and in the templates it calls, sees the
 * result document's URI; close calls the handler's open, delivers the raw
 * result as the target asks and calls the handler's close, so the handler
 * sees result documents one after the other, in the order they complete
 * (a nested one first, D112). A result document whose content fails is
 * dropped at a later close or at the end of the transformation; the
 * handler never sees it.
 *
 * Temporary output state (26.2.6, XTDE1480): lexically inside
 * xsl:variable, xsl:param, xsl:with-param, xsl:function, xsl:key,
 * xsl:sort, xsl:accumulator-rule or xsl:merge-key, the instruction
 * raises XTDE1480; dynamically, the templates such an instruction calls
 * receive the tunnel parameter Q{urn:x-sxml:output}temporary-output-state
 * (put in by src/xslt_lower_template.c), which the instruction tests; a
 * function call invocation sets the writer's temporary output state.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* The namespace of the tunnel parameter that marks temporary output
   state. */
#define SXML_XSLT_OUTPUT_STATE_NAMESPACE "urn:x-sxml:output"
#define SXML_XSLT_OUTPUT_STATE_LOCAL_NAME "temporary-output-state"

/* ------------------------------------------------------ Stylesheet outputs */

typedef struct sxml_xslt_stylesheet_outputs sxml_xslt_stylesheet_outputs;

typedef struct sxml_xslt_stylesheet_outputs_configuration {
    /* Sealed; borrowed for the call: nothing of it is kept. */
    const sxml_xslt_ast *ast;
    /* Its heap, allocator and resolver (parameter documents, kind
       SXML_RESOURCE_KIND_XML_DOCUMENT); kept by reference, must outlive
       the outputs. */
    sxml_processor *processor;
    /* Receives a static error; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_xslt_stylesheet_outputs_configuration;

/* Writes the requirements of the fixed part to *requirements; what the
   outputs hold is allocated through the processor's allocator.
   SXML_STATUS_INVALID_ARGUMENT without an AST or processor. */
SXML_NODISCARD sxml_status sxml_xslt_stylesheet_outputs_memory_requirements(
    const sxml_xslt_stylesheet_outputs_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Compiles the output definitions, named character maps and result
 * document instructions of the AST into memory and writes the address to
 * *outputs. Returns SXML_STATUS_STATIC_ERROR with the error record filled
 * (XTSE1560, XTSE1580, XTSE1590, XTSE1600, and SEPM0016, SESU0007,
 * SESU0011, SEPM0017 to SEPM0019 of a declaration or its parameter
 * document as static errors at the declaration), the memory checks of the
 * conventions, and the failures of the allocator, the resolver and the
 * name pool; *outputs is then untouched and nothing is left allocated.
 */
SXML_NODISCARD sxml_status sxml_xslt_stylesheet_outputs_initialize(
    sxml_memory_view memory, const sxml_xslt_stylesheet_outputs_configuration *configuration,
    sxml_xslt_stylesheet_outputs **outputs);

/* Releases what the outputs hold; accepts NULL. */
void sxml_xslt_stylesheet_outputs_finalize(sxml_xslt_stylesheet_outputs *outputs);

/* Allocates through allocator (stored, must outlive the outputs) and
   initializes. */
SXML_NODISCARD sxml_status sxml_xslt_stylesheet_outputs_create(
    sxml_allocator allocator, const sxml_xslt_stylesheet_outputs_configuration *configuration,
    sxml_xslt_stylesheet_outputs **outputs);

/* Finalizes outputs made by create and frees them; accepts NULL. */
void sxml_xslt_stylesheet_outputs_destroy(sxml_xslt_stylesheet_outputs *outputs);

/*
 * Writes the output definition named output_definition_name (0: the
 * unnamed one, which always exists) to *output_definition, its views
 * borrowed from the outputs, as sxml_xslt_stylesheet_get_output_definition
 * describes. SXML_STATUS_INVALID_ARGUMENT when there is none of that name;
 * *output_definition is then untouched.
 */
SXML_NODISCARD sxml_status sxml_xslt_stylesheet_outputs_get_definition(
    const sxml_xslt_stylesheet_outputs *outputs, sxml_name_identifier output_definition_name,
    sxml_xslt_output_definition *output_definition);

/* Writes the mappings of the named character map character_map_name to
   *character_map (borrowed), sorted by codepoint; false when the
   stylesheet has none of that name (fn:character-map, 26.1.7). */
typedef struct sxml_xslt_character_map_answer {
    bool is_found;
    sxml_character_map_view character_map;
} sxml_xslt_character_map_answer;

sxml_xslt_character_map_answer sxml_xslt_stylesheet_outputs_find_character_map(
    const sxml_xslt_stylesheet_outputs *outputs, sxml_name_identifier character_map_name);

/* ------------------------------------------------------------ Delivery */

/* A raw result to deliver with an output definition. */
typedef struct sxml_xslt_result_request {
    /* Borrowed. */
    sxml_sequence value;
    /* Borrowed for the call. */
    const sxml_xslt_output_definition *output_definition;
    /* The result's URI: the base URI of a built tree; may be empty. */
    sxml_byte_view result_uri;
    /* The principal result of a stylesheet whose principal module has
       version 1.0: an XHTML html element then chooses the xml method
       (26.1.2). */
    bool prefers_xml_for_xhtml;
    sxml_error_record *error_record;
} sxml_xslt_result_request;

/*
 * Serializes request->value with its output definition to output_stream:
 * the method the definition gives or the one chosen from the value
 * (26.1.2), indent DEFAULT as yes for html and xhtml (26.1.4). Returns
 * the failures of sxml_serialize_sequence.
 */
SXML_NODISCARD sxml_status sxml_xslt_serialize_result(sxml_item_heap *item_heap,
                                                      const sxml_xslt_result_request *request,
                                                      sxml_output_stream output_stream);

/*
 * Writes, owned, the document node sequence normalization makes of
 * request->value (Serialization 4.0, section 2, with the definition's
 * item-separator; 2.4.6.1) to *document, its base URI request->result_uri.
 * SENR0001 for an attribute or namespace node, a map or another function
 * item; the heap's and the allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_xslt_build_result_tree(sxml_item_heap *item_heap,
                                                       const sxml_xslt_result_request *request,
                                                       sxml_sequence *document);

/* Whether a result delivered by default is a tree: the effective
   build-tree of the definition for value (26.1). */
typedef struct sxml_xslt_tree_choice {
    bool builds_tree;
} sxml_xslt_tree_choice;

SXML_NODISCARD sxml_status sxml_xslt_result_builds_tree(sxml_item_heap *item_heap,
                                                        const sxml_xslt_result_request *request,
                                                        sxml_xslt_tree_choice *choice);

/* ---------------------------------------------------------- Result writer */

typedef struct sxml_xslt_result_writer sxml_xslt_result_writer;

typedef struct sxml_xslt_result_writer_configuration {
    /* Kept by reference; must outlive the writer. */
    const sxml_xslt_stylesheet_outputs *outputs;
    sxml_processor *processor;
    /* Receives the result documents; the null handler discards them. */
    sxml_xslt_result_document_handler result_document_handler;
    /* An absolute URI, or empty; copied. */
    sxml_byte_view base_output_uri;
    /* A function call invocation (2.4.5): the whole transformation is in
       temporary output state. */
    bool is_in_temporary_output_state;
    /* The principal module's version is 1.0 (26.1.2). */
    bool prefers_xml_for_xhtml;
} sxml_xslt_result_writer_configuration;

SXML_NODISCARD sxml_status sxml_xslt_result_writer_memory_requirements(
    const sxml_xslt_result_writer_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places a writer with no result document yet in memory. Allocates as
   result documents come, through the processor's allocator. */
SXML_NODISCARD sxml_status sxml_xslt_result_writer_initialize(
    sxml_memory_view memory, const sxml_xslt_result_writer_configuration *configuration,
    sxml_xslt_result_writer **writer);

/* Drops the result documents still open and releases the writer's
   memory; accepts NULL. */
void sxml_xslt_result_writer_finalize(sxml_xslt_result_writer *writer);

SXML_NODISCARD sxml_status sxml_xslt_result_writer_create(
    sxml_allocator allocator, const sxml_xslt_result_writer_configuration *configuration,
    sxml_xslt_result_writer **writer);

void sxml_xslt_result_writer_destroy(sxml_xslt_result_writer *writer);

/* The principal result to deliver after the evaluation. */
typedef struct sxml_xslt_principal_result_request {
    /* The raw result, borrowed. */
    sxml_sequence value;
    /* NULL: the unnamed output definition. */
    const sxml_xslt_output_definition *output_definition;
    sxml_xslt_result_delivery result_delivery;
    /* SERIALIZED: receives the bytes. */
    sxml_output_stream output_stream;
    sxml_error_record *error_record;
} sxml_xslt_principal_result_request;

/*
 * Drops the result documents left open, checks XTDE1490 for
 * the principal result (a result document with the base output URI and a
 * principal result that is not empty; an empty one is discarded, 26.2.2)
 * and delivers it: *principal_result receives, owned, the raw result
 * (RAW), one document node (TREE, DEFAULT with build-tree yes; its base
 * URI the base output URI) or the empty sequence (SERIALIZED, written to
 * the output stream). Returns SXML_STATUS_DYNAMIC_ERROR with the record
 * filled for XTDE1490 and serialization errors, the stream's and the
 * handler's failures; *principal_result is then untouched.
 */
SXML_NODISCARD sxml_status sxml_xslt_result_writer_deliver_principal_result(
    sxml_xslt_result_writer *writer, const sxml_xslt_principal_result_request *request,
    sxml_sequence *principal_result);

/* The current output URI (26.3): the absolute URI of the innermost result
   document being written, else the base output URI; absent in temporary
   output state and when the base output URI is absent. */
typedef struct sxml_xslt_current_output_uri {
    bool is_present;
    /* Borrowed from the writer until the next result document. */
    sxml_byte_view uri;
} sxml_xslt_current_output_uri;

sxml_xslt_current_output_uri sxml_xslt_result_writer_get_current_output_uri(
    const sxml_xslt_result_writer *writer);

/* The stylesheet outputs the writer serves. */
const sxml_xslt_stylesheet_outputs *sxml_xslt_result_writer_get_outputs(
    const sxml_xslt_result_writer *writer);

/* True when a result document without href (or with the base output
   URI) became the principal result (26.2.2). */
bool sxml_xslt_result_writer_has_principal_document(const sxml_xslt_result_writer *writer);

/* The writer of the transformation a call runs in: NULL outside one. */
sxml_xslt_result_writer *sxml_xslt_result_writer_from_call(const sxml_function_call *call);
