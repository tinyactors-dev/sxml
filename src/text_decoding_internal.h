#pragma once

/*
 * Text decoding (glossary `text_decoding`): turning the octets of a text
 * resource or a binary value into a string, by an effective encoding and a
 * start offset chosen from a byte order mark and an encoding candidate.
 * One decoder for fn:unparsed-text, fn:unparsed-text-lines,
 * fn:unparsed-text-available and fn:json-doc (F&O 4.0, sections 17.1.5 to
 * 17.1.7 and 17.4.5; src/function_library_resource.c) and for
 * bin:infer-encoding and bin:decode-string (EXPath Binary 4.0, sections
 * 6.1 and 6.2; src/function_library_binary.c). Internal to sxml; defined in
 * src/text_decoding.c.
 *
 * The encodings are the XML parser's (D51): UTF-8, UTF-16 in either byte
 * order, ISO-8859-1 and US-ASCII; encoding names are classified by
 * sxml_xml_classify_encoding_name.
 *
 * Thread safety: no state; the text being built is the caller's.
 */
#include "function_library_core_internal.h"
#include "sxml/status.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>

/* The encodings text is decoded from (the XML parser's, D51). */
typedef enum sxml_text_encoding {
    SXML_TEXT_ENCODING_UTF8,
    SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN,
    SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN,
    SXML_TEXT_ENCODING_ISO_8859_1,
    SXML_TEXT_ENCODING_US_ASCII,
} sxml_text_encoding;

/* How to decode: the effective encoding and where the characters start. */
typedef struct sxml_text_decoding {
    sxml_text_encoding encoding;
    /* The offset, in octets, after a byte order mark the rules consumed;
       0 when there is none. */
    size_t start_offset;
    /* True when the encoding came from a candidate, false when inferred
       (fn:unparsed-text raises FOUT1190 or FOUT1200 accordingly). */
    bool has_candidate;
} sxml_text_decoding;

/* The octets to decode and the encoding named for them. */
typedef struct sxml_text_decoding_choice {
    sxml_byte_view content;
    /* The encoding candidate; empty when none is named. */
    sxml_byte_view candidate;
} sxml_text_decoding_choice;

/*
 * Chooses the effective encoding and start offset (bin:infer-encoding,
 * EXPath Binary 4.0, section 6.1; F&O 4.0, section 17.1.5): without a
 * candidate a byte order mark decides, else UTF-8; a mark is skipped when
 * it agrees with the candidate; a candidate "UTF-16" without one is
 * big-endian. A mark the candidate disagrees with is decoded as characters
 * of the candidate. Returns false for a candidate sxml does not support.
 */
bool sxml_text_decoding_choose(sxml_text_decoding_choice choice,
                               sxml_text_decoding *decoding);

/* Why decoding gave no string. */
typedef enum sxml_text_decoding_failure {
    SXML_TEXT_DECODING_FAILURE_NONE,
    /* Octets that do not decode in a candidate encoding, or a character
       that is not permitted. */
    SXML_TEXT_DECODING_FAILURE_ENCODING,
    /* No candidate, and octets that are not UTF-8. */
    SXML_TEXT_DECODING_FAILURE_UNDECODABLE,
} sxml_text_decoding_failure;

/* What to decode and by which rules. */
typedef struct sxml_text_decoding_request {
    sxml_byte_view content;
    sxml_text_decoding decoding;
    /* CR LF and a lone CR become LF (F&O 4.0, 17.1.5, normalize-newlines). */
    bool normalizes_newlines;
    /* U+FFFD replaces what does not decode or is not permitted (F&O 4.0,
       17.1.5, fallback); otherwise that is a failure. */
    bool has_fallback;
    /* Every codepoint passes (fn:json-doc, F&O 4.0, 17.4.5); otherwise
       only XML characters ([2] Char). */
    bool permits_any_character;
} sxml_text_decoding_request;

/*
 * Decodes request->content from request->decoding.start_offset on and
 * appends the characters to *text. A failure the rules do not repair is
 * written to *failure (NONE otherwise), with the status OK; what was
 * appended before stays in *text. Returns the text's failures (the heap's,
 * SXML_STATUS_ARITHMETIC_OVERFLOW).
 */
SXML_NODISCARD sxml_status sxml_text_decode(const sxml_text_decoding_request *request,
                                            sxml_library_text *text,
                                            sxml_text_decoding_failure *failure);
