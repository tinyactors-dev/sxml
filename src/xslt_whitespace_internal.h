#pragma once

#include <stdbool.h>

#include "processor_internal.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/xdm.h"
#include "xslt_ast_internal.h"

/*
 * Whitespace stripping of source documents (XSLT 4.0, 4.2.2; packet 4.12,
 * docs/DECISIONS.md D150), and the stripping of their type annotations
 * that input-type-annotations="strip" on a module asks for (4.1; D179):
 * the same copy, without the annotations. Internal to sxml.
 *
 * The xsl:strip-space and xsl:preserve-space declarations of a stylesheet
 * decide, for each element name, whether the whitespace text nodes among
 * an element's children are removed from source documents: the best
 * matching name test wins (higher import precedence, then higher default
 * priority, then later in declaration order), an element with no match
 * keeps them, and xml:space="preserve" on the element or its nearest
 * ancestor with xml:space keeps them too. As the specification puts it,
 * the processor "makes a copy of the source tree from which unwanted
 * whitespace text nodes have been removed": sxml_xslt_strip_document copies
 * a document through a tree builder that drops them, so a parsed tree is
 * never changed and nothing recurses (sxml_tree_builder_copy_node walks
 * the tree in a loop).
 *
 * The documents that get stripped (4.2) are the global context item's (the
 * host strips it before the transformation) and those fn:doc and
 * document() load, through the document preparation of the evaluation's
 * document loader (src/processor_internal.h), which
 * sxml_xslt_whitespace_stripping_as_document_preparation makes.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* The stripping one stylesheet asks for. A plain value; everything is
   borrowed and must outlive its uses. */
typedef struct sxml_xslt_whitespace_stripping {
    /* Sealed. */
    const sxml_xslt_ast *ast;
    /* Builds the copies: its allocator, item heap and tree limits. */
    sxml_processor *processor;
} sxml_xslt_whitespace_stripping;

/* True when some xsl:strip-space can remove a text node: without one,
   stripping changes nothing and no copy is made. */
bool sxml_xslt_strips_whitespace(const sxml_xslt_ast *ast);

/* True when some module root asks for input-type-annotations="strip"
   (XSLT 4.0, 4.2.1). */
bool sxml_xslt_strips_type_annotations(const sxml_xslt_ast *ast);

/* True when the stylesheet strips whitespace or type annotations: only
   then are documents copied (sxml_xslt_strip_document). */
bool sxml_xslt_prepares_documents(const sxml_xslt_ast *ast);

/*
 * Writes document_node, or (when it is a document node and the stylesheet
 * strips whitespace) a copy of its document without the whitespace text
 * nodes the declarations remove, to *stripped, owned (the caller releases
 * it; document_node is still the caller's). Returns the failures of
 * copying and of the allocator; *stripped is untouched on failure.
 */
SXML_NODISCARD sxml_status
sxml_xslt_strip_document(const sxml_xslt_whitespace_stripping *stripping,
                         sxml_item document_node, sxml_item *stripped);

/* The document preparation that strips every document a loader parses;
   stripping must outlive the loader. */
sxml_document_preparation sxml_xslt_whitespace_stripping_as_document_preparation(
    const sxml_xslt_whitespace_stripping *stripping);

/* XTSE0270 (4.2.2): one name test in an xsl:strip-space and an
   xsl:preserve-space of one import precedence. Returns
   SXML_STATUS_STATIC_ERROR with *conflict (a node of one of them) set, else
   SXML_STATUS_OK. */
SXML_NODISCARD sxml_status sxml_xslt_check_space_declarations(const sxml_xslt_ast *ast,
                                                             sxml_xslt_ast_node_index *conflict);
