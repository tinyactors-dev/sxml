#pragma once

/*
 * The HTML parser behind fn:parse-html and fn:html-doc (F&O 4.0, 17.3;
 * HTML Living Standard, 13.2 "Parsing HTML documents"; packet 7.10a,
 * D201). Internal to sxml; an optional feature (D189): only a program
 * that names sxml_html_parser_feature() links it.
 *
 * - src/html_tokenizer.c: the tokenizer (13.2.5), a loop over a state
 *   variable that the tree construction stage pulls tokens from.
 * - src/html_tree_construction.c: tree construction (13.2.6) on the HTML
 *   working tree (glossary `html_working_tree`): the insertion modes, the
 *   stack of open elements, the list of active formatting elements, the
 *   adoption agency algorithm and foster parenting. Nodes are moved after
 *   they are built, so the tree is a node table, not tree builder events.
 * - src/html.c: the input byte stream (13.2.3: decoding, the byte order
 *   mark, preprocessing), the walk that sends the working tree to a tree
 *   builder (F&O 4.0, 17.3.1, the XDM mapping), and the feature.
 * - src/function_library_html.c: the rows of fn:parse-html and
 *   fn:html-doc, the feature's library functions (D191).
 *
 * No recursion (D71): the tokenizer is a loop, the "process the token
 * using the rules for" steps of tree construction are a dispatch loop
 * over insertion modes, and the walk over the working tree follows its
 * parent links.
 *
 * Thread safety: the thread of the caller; nothing is shared.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "xml_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------- Tokens */

/* The kinds of token the tokenizer emits (13.2.5). Character tokens come
   in runs: one CHARACTERS token holds every character emitted between two
   other tokens. */
typedef enum sxml_html_token_kind {
    SXML_HTML_TOKEN_KIND_CHARACTERS,
    SXML_HTML_TOKEN_KIND_START_TAG,
    SXML_HTML_TOKEN_KIND_END_TAG,
    SXML_HTML_TOKEN_KIND_COMMENT,
    SXML_HTML_TOKEN_KIND_DOCTYPE,
    SXML_HTML_TOKEN_KIND_PROCESSING_INSTRUCTION,
    SXML_HTML_TOKEN_KIND_END_OF_FILE,
} sxml_html_token_kind;

/* One attribute of a tag token, as tokenized: the name in ASCII lower
   case, the value with character references replaced. */
typedef struct sxml_html_token_attribute {
    sxml_byte_view attribute_name;
    sxml_byte_view attribute_value;
} sxml_html_token_attribute;

typedef struct sxml_html_token_attribute_view {
    const sxml_html_token_attribute *attributes;
    size_t attribute_count;
} sxml_html_token_attribute_view;

/* A token. Views are owned by the tokenizer and valid until its next
   call. Fields a kind does not use are empty. */
typedef struct sxml_html_token {
    sxml_html_token_kind token_kind;
    /* CHARACTERS: the characters, UTF-8; comment and processing
       instruction data. */
    sxml_byte_view data;
    /* Tag name, DOCTYPE name, processing instruction target. */
    sxml_byte_view name;
    /* START_TAG and END_TAG; duplicate attributes already dropped
       (13.2.5.33). */
    sxml_html_token_attribute_view attributes;
    bool is_self_closing;
    /* DOCTYPE: which parts are present ("missing" is not empty). */
    bool has_name;
    bool has_public_identifier;
    bool has_system_identifier;
    bool forces_quirks;
    sxml_byte_view public_identifier;
    sxml_byte_view system_identifier;
} sxml_html_token;

/* The states tree construction switches the tokenizer to (13.2.6). */
typedef enum sxml_html_content_state {
    SXML_HTML_CONTENT_STATE_DATA,
    SXML_HTML_CONTENT_STATE_RCDATA,
    SXML_HTML_CONTENT_STATE_RAWTEXT,
    SXML_HTML_CONTENT_STATE_SCRIPT_DATA,
    SXML_HTML_CONTENT_STATE_PLAINTEXT,
} sxml_html_content_state;

/* ---------------------------------------------------------- Tokenizer */

/*
 * The tokenizer's state. Its fields are private to src/html_tokenizer.c;
 * the struct is here so that the parser can hold it by value.
 */
typedef struct sxml_html_tokenizer {
    /* Preprocessed input (13.2.3.5): well-formed UTF-8, no CR. Borrowed. */
    sxml_byte_view input;
    size_t input_offset;
    /* The byte count of the current input character, for reconsuming. */
    size_t current_byte_count;
    uint32_t current_character;
    uint8_t state;
    uint8_t return_state;
    /* The token being built and its text. */
    sxml_html_token_kind token_kind;
    sxml_xml_buffer token_text;
    sxml_byte_range name_range;
    sxml_byte_range data_range;
    sxml_byte_range public_identifier_range;
    sxml_byte_range system_identifier_range;
    bool has_name;
    bool has_public_identifier;
    bool has_system_identifier;
    bool forces_quirks;
    bool is_self_closing;
    /* sxml_html_attribute_record entries of the tag being built. */
    sxml_xml_array attribute_records;
    /* sxml_html_token_attribute entries of the emitted tag. */
    sxml_xml_array emitted_attributes;
    /* The characters emitted since the last other token. */
    sxml_xml_buffer characters;
    sxml_xml_buffer temporary_buffer;
    sxml_xml_buffer last_start_tag_name;
    uint32_t character_reference_code;
    /* A token other than characters waits to be handed out. */
    bool has_ready_token;
    /* The characters were handed out; the next call clears them. */
    bool has_handed_out_characters;
    bool has_reached_end;
    bool has_emitted_end_of_file;
    /* Set by tree construction: the adjusted current node is not in the
       HTML namespace, so "<![CDATA[" opens a CDATA section (13.2.5.42). */
    bool allows_cdata_sections;
    /* A parse error was found (13.2.2). */
    bool has_parse_error;
} sxml_html_tokenizer;

/* Places a tokenizer over input, which must outlive it, in the data
   state. It allocates through allocator as it reads. */
typedef struct sxml_html_tokenizer_configuration {
    sxml_allocator allocator;
    sxml_byte_view input;
} sxml_html_tokenizer_configuration;

void sxml_html_tokenizer_initialize(sxml_html_tokenizer *tokenizer,
                                    const sxml_html_tokenizer_configuration *configuration);

/* Releases the tokenizer's buffers. Accepts NULL. */
void sxml_html_tokenizer_finalize(sxml_html_tokenizer *tokenizer);

/*
 * Writes the next token to *token: a run of characters, another token or,
 * at the end and on every later call, END_OF_FILE. Returns the
 * allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_html_tokenizer_next(sxml_html_tokenizer *tokenizer,
                                                    sxml_html_token *token);

/* Switches the tokenizer to state, as tree construction does after a
   start tag (13.2.6.2). */
void sxml_html_tokenizer_switch_state(sxml_html_tokenizer *tokenizer,
                                      sxml_html_content_state state);

/* --------------------------------------------------------- Working tree */

/* No node: the end of a list, a missing parent. */
#define SXML_HTML_NO_NODE UINT32_MAX

typedef enum sxml_html_node_kind {
    SXML_HTML_NODE_KIND_DOCUMENT,
    SXML_HTML_NODE_KIND_ELEMENT,
    SXML_HTML_NODE_KIND_TEXT,
    SXML_HTML_NODE_KIND_COMMENT,
    SXML_HTML_NODE_KIND_PROCESSING_INSTRUCTION,
} sxml_html_node_kind;

/* The namespaces of elements the HTML parser makes (13.1.2). */
typedef enum sxml_html_namespace {
    SXML_HTML_NAMESPACE_HTML,
    SXML_HTML_NAMESPACE_MATHML,
    SXML_HTML_NAMESPACE_SVG,
} sxml_html_namespace;

/* The namespaces of attributes: none, or those of "adjust foreign
   attributes" (13.2.6.1). */
typedef enum sxml_html_attribute_namespace {
    SXML_HTML_ATTRIBUTE_NAMESPACE_NONE,
    SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK,
    SXML_HTML_ATTRIBUTE_NAMESPACE_XML,
    SXML_HTML_ATTRIBUTE_NAMESPACE_XMLNS,
} sxml_html_attribute_namespace;

/* An attribute in the working tree. Ranges are of the tree's text. */
typedef struct sxml_html_attribute {
    /* The local name: after "adjust foreign attributes", without its
       prefix. */
    sxml_byte_range attribute_name;
    sxml_byte_range attribute_value;
    sxml_html_attribute_namespace attribute_namespace;
} sxml_html_attribute;

/* A slice of the tree's text that a text node holds; text nodes are
   chains of segments, so that text inserted later joins them. */
typedef struct sxml_html_text_segment {
    sxml_byte_range text_range;
    uint32_t next_segment;
} sxml_html_text_segment;

/* A node of the working tree. Links are node numbers, SXML_HTML_NO_NODE
   for none. */
typedef struct sxml_html_node {
    sxml_html_node_kind node_kind;
    sxml_html_namespace element_namespace;
    /* An sxml_html_tag value: the element's tag name as tokenized, for the
       checks of tree construction; 0 (other) for names it does not name. */
    uint32_t tag;
    uint32_t parent;
    uint32_t first_child;
    uint32_t last_child;
    uint32_t previous_sibling;
    uint32_t next_sibling;
    /* Element: its local name; processing instruction: its target. */
    sxml_byte_range name;
    /* Comment and processing instruction data. */
    sxml_byte_range data;
    /* Text: the first and last segments. */
    uint32_t first_segment;
    uint32_t last_segment;
    /* Element: its attributes in the tree's attribute table. */
    uint32_t first_attribute;
    uint32_t attribute_count;
    /* A MathML annotation-xml element whose encoding makes it an HTML
       integration point (13.2.6). */
    bool is_html_integration_point;
    /* The element is on the stack of open elements (13.2.4.2). */
    bool is_open;
} sxml_html_node;

/* The working tree: tables in allocator memory, node 0 the document. */
typedef struct sxml_html_working_tree {
    sxml_xml_array nodes;
    sxml_xml_array attributes;
    sxml_xml_array segments;
    sxml_xml_buffer text;
} sxml_html_working_tree;

/* ------------------------------------------------------------- Parsing */

/* What to parse. */
typedef struct sxml_html_parse_request {
    /* Preprocessed input (sxml_html_prepare_input). Borrowed. */
    sxml_byte_view input;
    /* Stop at the first parse error (fn:parse-html's fail-on-error). */
    bool stops_at_parse_error;
    /* The encoding the input was decoded with, and whether the confidence
       is tentative (13.2.3.2): then a meta element that declares another
       encoding stops parsing to change the encoding (13.2.3.4). */
    uint8_t encoding_number;
    bool is_encoding_tentative;
} sxml_html_parse_request;

/* What parsing found. */
typedef struct sxml_html_parse_outcome {
    /* A parse error was found (13.2.2); with stops_at_parse_error the
       tree is then incomplete and must not be used. */
    bool has_parse_error;
    /* A meta element declared another encoding while the confidence was
       tentative (13.2.3.4): parsing stopped, the tree is incomplete, and
       the input is to be parsed again in new_encoding_number with the
       confidence certain. */
    bool changes_encoding;
    uint8_t new_encoding_number;
} sxml_html_parse_outcome;

/*
 * Tokenizes request->input and constructs *tree (an empty working tree
 * over an allocator, sxml_html_working_tree_initialize), as an HTML
 * document without scripting (13.2.6). Returns the allocator's failures;
 * the tree is then incomplete and is finalized by the caller.
 */
SXML_NODISCARD sxml_status sxml_html_construct_tree(const sxml_html_parse_request *request,
                                                    sxml_html_working_tree *tree,
                                                    sxml_html_parse_outcome *outcome);

/* An empty working tree over allocator: the document node alone. Returns
   the allocator's failures. */
SXML_NODISCARD sxml_status sxml_html_working_tree_initialize(sxml_html_working_tree *tree,
                                                             sxml_allocator allocator);

/* Releases the tree's tables. */
void sxml_html_working_tree_finalize(sxml_html_working_tree *tree);

/* The node with number node_number, which must exist. */
sxml_html_node *sxml_html_working_tree_node(const sxml_html_working_tree *tree,
                                            uint32_t node_number);

/* ------------------------------------------------------- Input stream */

/* Bytes or a string to turn into the input stream (13.2.3). */
typedef struct sxml_html_input_request {
    sxml_byte_view input;
    /* True for an xs:string: UTF-8 with a known definite encoding (F&O
       4.0, fn:parse-html; 13.2.3.1); a leading U+FEFF is stripped. False
       for octets, whose encoding is determined (13.2.3.2). */
    bool is_string;
    /* fn:parse-html's encoding option, or empty: an override of the
       encoding for octets (13.2.3.2, step 2). */
    sxml_byte_view encoding_name;
    /* Octets are decoded in known_encoding_number with the confidence
       certain, without sniffing: the second pass after a change of the
       encoding (13.2.3.4). */
    bool has_known_encoding;
    uint8_t known_encoding_number;
} sxml_html_input_request;

/* Why the input stream could not be made. */
typedef enum sxml_html_input_failure {
    SXML_HTML_INPUT_FAILURE_NONE,
    /* The encoding option names no encoding sxml decodes. */
    SXML_HTML_INPUT_FAILURE_UNKNOWN_ENCODING,
} sxml_html_input_failure;

/* The input stream: well-formed UTF-8 with newlines normalized
   (13.2.3.5), in *stream. */
typedef struct sxml_html_input_outcome {
    sxml_html_input_failure failure;
    /* The encoding decoded with, and whether the confidence is tentative
       (13.2.3.2): octets whose encoding was sniffed, not given by a byte
       order mark, the encoding option or known_encoding_number. A string
       is UTF-8 with the confidence certain. */
    uint8_t encoding_number;
    bool is_encoding_tentative;
    /* A preprocessing parse error: a surrogate (decoded from UTF-16), a
       noncharacter or a control in the stream (13.2.3.5). */
    bool has_parse_error;
} sxml_html_input_outcome;

/*
 * Decodes request->input into *stream (an empty buffer over an allocator)
 * and normalizes its newlines. Invalid byte sequences become U+FFFD, as
 * the Encoding Standard's decoders do. Returns the buffer's failures.
 */
SXML_NODISCARD sxml_status sxml_html_prepare_input(const sxml_html_input_request *request,
                                                   sxml_xml_buffer *stream,
                                                   sxml_html_input_outcome *outcome);

/* The encodings of the WHATWG Encoding Standard, every one of which
   decodes octets here: numbers 0 to 4 below, then the 28 legacy
   single-byte encodings in the standard's order, then the legacy
   multi-byte ones (src/html_encoding_tables.c, generated by
   tools/generate-html-encodings.py, which checks the numbers). */
#define SXML_HTML_ENCODING_UTF_8 0U
#define SXML_HTML_ENCODING_UTF_16BE 1U
#define SXML_HTML_ENCODING_UTF_16LE 2U
#define SXML_HTML_ENCODING_REPLACEMENT 3U
#define SXML_HTML_ENCODING_X_USER_DEFINED 4U
#define SXML_HTML_ENCODING_FIRST_SINGLE_BYTE 5U
#define SXML_HTML_ENCODING_WINDOWS_1252 25U
#define SXML_HTML_ENCODING_GBK 33U
#define SXML_HTML_ENCODING_GB18030 34U
#define SXML_HTML_ENCODING_BIG5 35U
#define SXML_HTML_ENCODING_EUC_JP 36U
#define SXML_HTML_ENCODING_ISO_2022_JP 37U
#define SXML_HTML_ENCODING_SHIFT_JIS 38U
#define SXML_HTML_ENCODING_EUC_KR 39U

/* An encoding label in ASCII lower case, and its encoding's number. */
typedef struct sxml_html_encoding_label {
    sxml_byte_view label;
    uint8_t encoding_number;
} sxml_html_encoding_label;

/* Sorted by the bytes of the label. */
extern const sxml_html_encoding_label sxml_html_encoding_labels[];
extern const size_t sxml_html_encoding_label_count;
/* The codepoints of bytes 0x80 to 0xFF of each single-byte encoding; 0
   where the byte decodes to an error. */
extern const uint16_t sxml_html_single_byte_codepoints[][128];
extern const size_t sxml_html_single_byte_encoding_count;

/* The indexes of the multi-byte encodings (Encoding Standard, 5): the
   code point of each pointer, 0 for none. Big5's start at pointer 942;
   for the pointers of sxml_html_big5_plane_2_pointers the code point is
   U+20000 plus the entry. */
enum {
    SXML_HTML_INDEX_JIS0208_COUNT = 11104,
    SXML_HTML_INDEX_JIS0212_COUNT = 7211,
    SXML_HTML_INDEX_EUC_KR_COUNT = 23750,
    SXML_HTML_INDEX_GB18030_COUNT = 23940,
    SXML_HTML_INDEX_BIG5_FIRST_POINTER = 942,
    SXML_HTML_INDEX_BIG5_COUNT = 18840,
    SXML_HTML_BIG5_PLANE_2_COUNT = 1713,
    SXML_HTML_GB18030_RANGE_COUNT = 207,
};
extern const uint16_t sxml_html_index_jis0208[SXML_HTML_INDEX_JIS0208_COUNT];
extern const uint16_t sxml_html_index_jis0212[SXML_HTML_INDEX_JIS0212_COUNT];
extern const uint16_t sxml_html_index_euc_kr[SXML_HTML_INDEX_EUC_KR_COUNT];
extern const uint16_t sxml_html_index_gb18030[SXML_HTML_INDEX_GB18030_COUNT];
extern const uint16_t sxml_html_index_big5[SXML_HTML_INDEX_BIG5_COUNT];
extern const uint16_t sxml_html_big5_plane_2_pointers[SXML_HTML_BIG5_PLANE_2_COUNT];
/* index gb18030 ranges: sorted pointers and the code points they start. */
extern const uint32_t sxml_html_gb18030_range_pointers[SXML_HTML_GB18030_RANGE_COUNT];
extern const uint32_t sxml_html_gb18030_range_codepoints[SXML_HTML_GB18030_RANGE_COUNT];

/* What a label names. */
typedef struct sxml_html_encoding_lookup {
    bool is_found;
    uint8_t encoding_number;
} sxml_html_encoding_lookup;

/* Encoding Standard, 4.2 "get an encoding": the label without leading
   and trailing ASCII whitespace, compared in ASCII lower case. */
sxml_html_encoding_lookup sxml_html_get_encoding(sxml_byte_view label);

/* HTML, 2.5.3 "the algorithm for extracting a character encoding from a
   meta element", for the value of a content attribute. */
sxml_html_encoding_lookup sxml_html_extract_meta_encoding(sxml_byte_view content);

/* 13.2.3.2 "prescan a byte stream to determine its encoding" over the
   first 1024 bytes of input, falling back to "get an XML encoding" as the
   prescan says. Not found: failure. */
sxml_html_encoding_lookup sxml_html_prescan_encoding(sxml_byte_view input);

/* -------------------------------------------------------- The feature */

/* An HTML document to parse into a tree (fn:parse-html). */
typedef struct sxml_html_document_request {
    sxml_html_input_request input;
    bool fails_on_parse_error;
    /* The new document's base URI and document URI; may be empty. */
    sxml_byte_view base_uri;
    sxml_byte_view document_uri;
} sxml_html_document_request;

/* Why parsing gave no document. */
typedef enum sxml_html_document_failure {
    SXML_HTML_DOCUMENT_FAILURE_NONE,
    SXML_HTML_DOCUMENT_FAILURE_UNKNOWN_ENCODING,
    /* fails_on_parse_error and a parse error was found (FODC0011). */
    SXML_HTML_DOCUMENT_FAILURE_PARSE_ERROR,
} sxml_html_document_failure;

typedef struct sxml_html_document_outcome {
    sxml_html_document_failure failure;
    /* Owned when failure is NONE: the document node of a new tree. */
    sxml_item document_node;
} sxml_html_document_outcome;

/*
 * Parses an HTML document into a new tree of processor, whose limits
 * apply, and writes its document node to outcome. Returns the
 * allocator's and the tree's failures (SXML_STATUS_LIMIT_EXCEEDED at a
 * limit).
 */
SXML_NODISCARD sxml_status sxml_html_parse_document(sxml_processor *processor,
                                                    const sxml_html_document_request *request,
                                                    sxml_html_document_outcome *outcome);

/* The operations of the HTML feature (sxml_html_parser_feature). */
typedef struct sxml_html_feature_operations {
    sxml_status (*parse_document)(sxml_processor *processor,
                                  const sxml_html_document_request *request,
                                  sxml_html_document_outcome *outcome);
} sxml_html_feature_operations;

/* The rows of fn:parse-html and fn:html-doc (src/function_library_html.c),
   the feature's library functions (D191). */
struct sxml_native_function_view;
extern const struct sxml_native_function_view sxml_html_library_functions;

