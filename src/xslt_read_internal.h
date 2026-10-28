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
 * The stylesheet reader of XSLT 4.0 (docs/DECISIONS.md D102, D139).
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
 * Packages (3.4.2, 3.4.3): an xsl:use-package asks the resolver for kind
 * SXML_RESOURCE_KIND_XSLT_PACKAGE with the package name as uri_reference
 * and absolute_uri and its package-version (or "*") as package_version;
 * the package's principal module is read in place with relation
 * USED_PACKAGE, once per xsl:use-package, and the children of
 * xsl:override are declarations. A package asked for twice with the same
 * name and range is parsed once. XTSE3000 when the resolver delivers
 * nothing, XTSE3005 for a package that uses itself, XTSE3008 for an
 * xsl:use-package in an imported module.
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
    /* NULL, or the collector of a compilation with a report sink (D320):
       error_record must then be &error_collector->current, and every
       static error goes to the sink while reading goes on (an attribute
       or an element in error is left out). The AST is then made also when
       errors were reported; the caller asks the collector. */
    sxml_xslt_error_collector *error_collector;
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

/*
 * One XSLT declaration that is not in a stylesheet module: an xsl:function,
 * xsl:key, xsl:include, ... child of a Schematron schema (D315, "Foreign
 * XSLT"), read into an AST another front end builds, exactly as the reader
 * reads a declaration of a stylesheet module: the same nodes, attributes,
 * expressions and static contexts, its includes and imports read in place
 * as modules under request.module, which must be the last module added
 * or one of its ancestors. Views are borrowed for the call.
 */
typedef struct sxml_xslt_declaration_request {
    sxml_processor *processor;
    /* Not sealed. */
    sxml_xslt_ast *ast;
    /* The module whose tree holds element. */
    sxml_xslt_ast_module_index module;
    /* The node the declaration becomes a child of (a module root), and
       the element it stands for in the module's tree: the declaration's
       parent element. */
    sxml_xslt_ast_node_index parent;
    sxml_node_index parent_element;
    /* The static context the declaration's is derived from: version,
       default mode, excluded namespaces, base URI. */
    sxml_xslt_ast_static_context_index parent_static_context;
    /* The declaration, a child of parent_element. */
    sxml_node_index element;
    /* Namespace bindings for the prefixes the declaration's own in-scope
       namespaces leave unbound (Schematron's sch:ns, 5.4.10). */
    sxml_namespace_binding_view extra_namespace_bindings;
    sxml_error_record *error_record;
    /* NULL, or as sxml_xslt_read_request.error_collector. */
    sxml_xslt_error_collector *error_collector;
    /* The element is read as an instruction of parent's sequence
       constructor (an xsl:copy-of in a Schematron property, a literal
       result element in a let's content), not as a declaration. */
    bool reads_instruction;
} sxml_xslt_declaration_request;

/*
 * Reads request->element into request->ast as a declaration under
 * request->parent (flag DECLARATION, appended to the declaration list), or
 * with reads_instruction as an instruction appended to parent's children.
 * An element the reader would not take as a declaration there is read as
 * it would be in a stylesheet (a data element, or XTSE0010). Returns the
 * reader's statuses; SXML_STATUS_INVALID_ARGUMENT for a sealed AST, a
 * module without a tree or no parent. On failure the AST may hold part
 * of the declaration: the caller discards it.
 */
SXML_NODISCARD sxml_status sxml_xslt_read_declaration(const sxml_xslt_declaration_request *request);
