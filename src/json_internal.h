#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * JSON texts (RFC 8259; F&O 4.0, section 17.4 "Functions on JSON Data";
 * docs/DECISIONS.md, D82). Internal to sxml.
 *
 * A JSON text is parsed with the tree-sitter grammar of grammars/json
 * (D12): any ERROR or MISSING node is err:FOJS0001. The concrete syntax
 * tree is walked once, without C recursion (D71), over an explicit stack
 * in allocator memory, and becomes either XDM values (fn:parse-json,
 * section 17.4.4) or tree builder events for the XML representation of
 * JSON (fn:json-to-xml, sections 17.4.2 and 17.4.6). The tree is freed
 * before the function returns. The reverse direction, the XML
 * representation written as a JSON text (fn:xml-to-json, section 17.4.7),
 * walks the node's subtree by node index, also without recursion.
 *
 * Strings follow F&O 4.0, section 17.4.3: a permitted character is a
 * codepoint of U+0001 to U+D7FF, U+E000 to U+FFFD or U+10000 to U+10FFFF
 * (the XML 1.1 characters, which xs:string holds). An escape sequence for
 * anything else (U+0000, an unpaired surrogate, U+FFFE, U+FFFF) becomes
 * the fallback's replacement for it, or U+FFFD, unless the escaped form is asked for, which keeps such
 * characters, the controls U+0000 to U+001F and U+007F to U+009F, and the
 * backslash as JSON escape sequences.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* What happens to the second and later members of an object with the
   same key (option "duplicates"). Keys are compared by codepoint after
   decoding, or in escaped form when the escaped form is asked for. */
typedef enum sxml_json_duplicates_policy {
    /* err:FOJS0003. */
    SXML_JSON_DUPLICATES_POLICY_REJECT,
    /* Later members with the key are dropped. */
    SXML_JSON_DUPLICATES_POLICY_USE_FIRST,
    /* fn:parse-json only: the last value wins, at the first's position
       (SXML_DUPLICATE_KEY_POLICY_USE_LAST of include/sxml/xdm.h). */
    SXML_JSON_DUPLICATES_POLICY_USE_LAST,
    /* fn:json-to-xml only: every member becomes an element. */
    SXML_JSON_DUPLICATES_POLICY_RETAIN,
} sxml_json_duplicates_policy;

/* How fn:parse-json makes numbers (option "number-format", F&O 4.0,
   section 17.4.4). */
typedef enum sxml_json_number_format {
    /* xs:double, as casting the lexical form gives it. */
    SXML_JSON_NUMBER_FORMAT_DOUBLE,
    /* xs:integer for an integral value without exponent, otherwise
       xs:decimal; with an exponent, the xs:double cast to xs:decimal. */
    SXML_JSON_NUMBER_FORMAT_DECIMAL,
    /* As DECIMAL without an exponent, xs:double with one. */
    SXML_JSON_NUMBER_FORMAT_ADAPTIVE,
} sxml_json_number_format;

/* What a fallback function gave for a character that is not permitted
   (option "fallback", F&O 4.0, sections 17.4.4 and 17.4.6): the codepoint
   an escape sequence stands for, and the string value of the function's
   result for its escape sequence, borrowed for the parse. */
typedef struct sxml_json_replacement {
    uint32_t codepoint;
    sxml_byte_view replacement_text;
} sxml_json_replacement;

typedef struct sxml_json_replacement_view {
    const sxml_json_replacement *replacements;
    size_t replacement_count;
} sxml_json_replacement_view;

/* The options of fn:parse-json and fn:json-to-xml after the function has
   checked them (the option parameter conventions, F&O 4.0, section 1.7). */
typedef struct sxml_json_parse_options {
    sxml_json_duplicates_policy duplicates_policy;
    /* Option "escape": special characters stay JSON escape sequences. */
    bool keeps_escaped_form;
    /* fn:parse-json only. */
    sxml_json_number_format number_format;
    /* fn:parse-json only: the value of JSON null (option "null"),
       borrowed for the call. */
    sxml_sequence null_value;
    /* The fallback's text for characters that are not permitted; one
       that has none becomes U+FFFD (the default fallback). */
    sxml_json_replacement_view replacements;
} sxml_json_parse_options;

/* An escape sequence for a character that is not permitted, found by
   sxml_json_find_unpermitted_escape. */
typedef struct sxml_json_unpermitted_escape {
    bool is_found;
    /* The codepoint it stands for: U+0000, an unpaired surrogate, U+FFFE
       or U+FFFF. */
    uint32_t codepoint;
    /* The six-character escape sequence as written, borrowed from the
       text. */
    sxml_byte_view escape_sequence;
    /* Where the next search starts. */
    size_t next_offset;
} sxml_json_unpermitted_escape;

/* Where to search. */
typedef struct sxml_json_escape_search {
    sxml_byte_view json_text;
    size_t start_offset;
} sxml_json_escape_search;

/*
 * Finds the first escape sequence at or after search.start_offset that
 * stands for a character that is not permitted, pairing surrogate escapes
 * as parsing does (RFC 8259, section 7), so that a function can call its
 * fallback for it before parsing. Lexical only: in a text that is not
 * valid JSON it may find escapes the parse then rejects (FOJS0001). Needs
 * no memory.
 */
sxml_json_unpermitted_escape sxml_json_find_unpermitted_escape(sxml_json_escape_search search);

/* A JSON text to parse. */
typedef struct sxml_json_text_request {
    /* Well-formed UTF-8, borrowed for the call. A leading byte order mark
       is skipped (F&O 4.0, section 17.4.4). */
    sxml_byte_view json_text;
    sxml_json_parse_options options;
    /* Memory for the concrete syntax tree walk (its stacks, decoded
       strings), all released before the call returns. Must not be the
       null allocator. The tree-sitter runtime itself allocates with
       malloc (D12). */
    sxml_allocator allocator;
    /* Most arrays and objects open at once; 0 means no bound but memory.
       Beyond it SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_depth;
    /* Receives FOJS0001 (with the line and column in the description) or
       FOJS0003; may be NULL. */
    sxml_error_record *error_record;
} sxml_json_text_request;

/*
 * Parses request->json_text as fn:parse-json does (F&O 4.0, section
 * 17.4.4) and writes the value, owned, to *value: a map with xs:string
 * keys in member order for an object, an array for an array, xs:string,
 * a number by the number format, xs:boolean, and the null value for
 * null. The duplicate policy must not be RETAIN.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR (FOJS0001, FOJS0003) with the error
 * record filled, SXML_STATUS_LIMIT_EXCEEDED beyond maximum_depth or a
 * text of 4 GiB or more, SXML_STATUS_INVALID_ARGUMENT for an invalid
 * request, and the heap's and allocator's failures. *value is untouched
 * on failure.
 */
SXML_NODISCARD sxml_status sxml_json_make_value(
    sxml_item_heap *item_heap, const sxml_json_text_request *request,
    sxml_sequence *value);

/*
 * Parses request->json_text as fn:json-to-xml does (F&O 4.0, sections
 * 17.4.2 and 17.4.6) and sends the events of the element that represents
 * its value to tree_builder: elements map, array, string, number, boolean
 * and null in the namespace http://www.w3.org/2005/xpath-functions with no
 * prefix (the outermost element binds it as the default namespace), key
 * attributes, and escaped="true" and escaped-key="true" where the escaped
 * form holds a backslash. Untyped. The caller sends start_document and
 * end_document around them. The duplicate policy must not be USE_LAST;
 * number_format and null_value are not used.
 *
 * Returns the failures of sxml_json_make_value and of tree_builder; on
 * failure the builder has received part of the events.
 */
SXML_NODISCARD sxml_status sxml_json_send_xml_events(
    sxml_item_heap *item_heap, const sxml_json_text_request *request,
    sxml_tree_builder tree_builder);

/* A node in the XML representation of JSON to write as a JSON text. */
typedef struct sxml_json_writing_request {
    /* A document or element node. */
    sxml_node_reference node;
    /* Option "escape-solidus" of fn:xml-to-json: write "/" as "\/". */
    bool escapes_solidus;
    /* Option "indent" of fn:xml-to-json: each member of a map or array on
       a line of its own, indented two spaces per level, and a space after
       a key's colon. */
    bool indents;
    /* Memory for the walk's stack and key index, released before the call
       returns. Must not be the null allocator. */
    sxml_allocator allocator;
    /* Receives FOJS0006 or FOJS0007; may be NULL. */
    sxml_error_record *error_record;
} sxml_json_writing_request;

/*
 * Writes request->node, which must be the XML representation of JSON, as
 * a JSON text without insignificant whitespace (fn:xml-to-json, F&O 4.0,
 * section 17.4.7) to output_stream. Numbers keep their lexical form,
 * adjusted to JSON syntax; strings and keys are escaped as the section
 * says.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR with FOJS0006 (not the XML
 * representation of JSON: a wrong name, attribute or content, a number
 * that is not an xs:double other than INF, -INF and NaN, two keys of one
 * map equal after unescaping) or FOJS0007 (an invalid escape sequence in
 * escaped form), the tree's, heap's and allocator's failures and those of
 * output_stream. On failure part of the text may have been written.
 */
SXML_NODISCARD sxml_status sxml_json_write_xml_representation(
    sxml_item_heap *item_heap, const sxml_json_writing_request *request,
    sxml_output_stream output_stream);
