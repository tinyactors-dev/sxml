#pragma once

/*
 * XInclude processing (glossary xinclude; XML Inclusions (XInclude) 1.0,
 * Second Edition; F&O 4.0, 17.1.1 and 17.2.1, the xinclude option of
 * fn:doc, fn:doc-available and fn:parse-xml, which XSLT 4.0's fn:document
 * shares, 20.1). Internal to sxml; defined in
 * src/function_library_xinclude.c.
 *
 * A document is copied into a new tree with every xi:include element
 * replaced by what it names: the children of the document fn:doc gives
 * for its href (parse="xml", resolved against the element's base URI, so
 * the loader's resolver, trust and stability apply), or the text
 * fn:unparsed-text would read (parse="text", with its encoding
 * attribute), or, when the resource cannot be read, the children of its
 * xi:fallback. Included documents are processed in the same walk: one
 * loop over an explicit stack of sibling cursors in heap memory, with no
 * recursion (D71); an include whose document is already being included
 * (or is the document itself) is an inclusion loop. The top elements of
 * an included document get xml:base and xml:lang attributes where their
 * base URI or language differs from the include's parent (glossary
 * inclusion_fixup; XInclude 1.0, 4.5.5 and 4.5.6).
 *
 * Not supported: the xpointer attribute (a resource error, so a fallback
 * applies) and the accept and accept-language attributes (ignored).
 *
 * Thread safety: D35, one thread per item heap.
 */
#include "function_library_core_internal.h"
#include "sxml/status.h"
#include "sxml/xdm.h"

#include <stdbool.h>

/* How to process one document's xi:include elements. */
typedef struct sxml_xinclude_request {
    /* The trust-external option: without it an xi:include element is
       FODC0016 (F&O 4.0, 17.1.1: XInclude elements fetch external
       resources). */
    bool trusts_external;
    /* Where errors go; NULL leaves them unreported (fn:doc-available). */
    sxml_error_record *error_record;
} sxml_xinclude_request;

/*
 * Replaces *document_node, an owned document node, by its XInclude
 * result, owned: the same node when it has no xi:include element, else a
 * new tree (released on failure, *document_node is then the empty item).
 * A fatal XInclude error (a misplaced xi:fallback, a parse attribute
 * other than "xml" or "text", an href with a fragment identifier or
 * empty, an inclusion loop, a resource that cannot be read without a
 * fallback) is FODC0002, or the loader's error for the resource.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR for those, and the heap's, the
 * tree's and the loader's failures.
 */
SXML_NODISCARD sxml_status sxml_function_library_process_xinclude(
    const sxml_function_call *call, const sxml_xinclude_request *request,
    sxml_item *document_node);
