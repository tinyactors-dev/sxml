#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xpath.h"
#include "xslt_ast_internal.h"

/*
 * The stylesheet reader of XSLT 4.0 (docs/DECISIONS.md D102, D138).
 * Module xslt_read, defined in src/xslt_read.c (packet 4.5). Internal to
 * sxml.
 *
 * Reads a principal stylesheet module and every module it includes and
 * imports into one stylesheet AST (src/xslt_ast_internal.h), doing what
 * that header lists under "What the reader has done before a node
 * exists". Each module is parsed by the processor's XML parser into an
 * arena tree with source locations (D81), then walked without recursion:
 * one explicit stack of open elements in allocator memory holds the
 * elements of every module being read, so a module met at an xsl:include
 * or xsl:import is read in place, depth first, before the elements after
 * that declaration (D102). Static expressions (use-when, static variables
 * and parameters, shadow attributes) are evaluated as they are met, in
 * stylesheet tree order, by src/xslt_static_internal.h.
 *
 * Modules come from the resource resolver with kind
 * SXML_RESOURCE_KIND_XSLT_MODULE: the href as uri_reference and resolved
 * against the base URI of the xsl:include or xsl:import as absolute_uri
 * (3.10.1). A module the resolver does not deliver, or that is not
 * well-formed, is XTSE0165. An xsl:import that would read a module whose
 * URI is already on the path of modules being read is XTSE0210 (3.10.3);
 * an xsl:include of such a module, or of a module already included in its
 * stylesheet level, is ignored (3.10.2: a duplicate include adds nothing,
 * so include-only cycles end there).
 *
 * Thread safety: the thread of the processor (D35).
 */

/* A stylesheet to read. Views are borrowed for the call. */
typedef struct sxml_xslt_read_request {
    /* Kept by reference by the AST (its item heap and allocator); must
       outlive it. */
    sxml_processor *processor;
    /* The principal module, an XML document in any encoding XML 1.0
       detects. */
    sxml_byte_view principal_module_text;
    /* The principal module's URI: its base URI and module URI, against
       which its includes and imports are resolved. Empty when it has
       none (relative hrefs then go to the resolver unresolved). */
    sxml_byte_view principal_module_uri;
    /* The values of static parameters by name (9.6), coerced to their
       declared types (XTTE0590). Names must be distinct. */
    sxml_variable_binding_view static_parameters;
    /* The default collation where no [xsl:]default-collation gives one;
       empty for the codepoint collation (3.6.2). */
    sxml_byte_view default_collation_uri;
    /* The null resolver means the processor's. */
    sxml_resource_resolver resource_resolver;
    /* Receives the first static error with its location (the module URI
       and the element's line and column, or the position inside an
       expression), or the dynamic error of a static expression; may be
       NULL. */
    sxml_error_record *error_record;
} sxml_xslt_read_request;

/*
 * Reads the stylesheet request describes and writes a sealed AST, owned
 * (release with sxml_xslt_ast_destroy), to *ast. The AST is made with
 * the processor's allocator and item heap and the processor's limits
 * bound its size: maximum_tree_node_count for the AST's nodes and
 * attributes, 1,000 modules.
 *
 * Returns SXML_STATUS_STATIC_ERROR (XTSE*, XPST*), SXML_STATUS_TYPE_ERROR
 * or SXML_STATUS_DYNAMIC_ERROR (a static expression's error, XTDE0050 for
 * a required static parameter without a value) with the error record
 * filled; SXML_STATUS_LIMIT_EXCEEDED; SXML_STATUS_INVALID_ARGUMENT for a
 * request without a processor; and the allocator's and resolver's
 * failures. Nothing is left allocated on failure; *ast is untouched.
 */
SXML_NODISCARD sxml_status sxml_xslt_read_stylesheet(const sxml_xslt_read_request *request,
                                                     sxml_xslt_ast **ast);
