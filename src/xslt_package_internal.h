#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "xslt_ast_internal.h"

/*
 * XSLT 4.0 packages (section 3.4; docs/DECISIONS.md D160; packet 4.16).
 * Internal to sxml.
 *
 * A package is a set of stylesheet modules whose components have a
 * visibility, as an XQuery library module is a set of declarations with
 * %private (D148): one AST holds every package of a stylesheet (the
 * reader reads a used package's principal module as a module of relation
 * USED_PACKAGE, once per xsl:use-package, src/xslt_read_internal.h), one
 * lowering lowers all their declarations into one unit, and this table
 * tells the lowering which declaration each reference binds to. Package 1
 * is the top-level package (the principal module's); every
 * USED_PACKAGE module starts a package whose parent is the package of
 * the module that uses it. Packages are numbered in module order, so a
 * used package comes after its user.
 *
 * Component instances (glossary): every package holds one instance per
 * component it can refer to (3.4.5): its own declarations that win by
 * import precedence (their visibility from xsl:expose and the visibility
 * attribute, 3.4.5.1; in an implicit package, an xsl:stylesheet, named
 * templates and modes are public; stylesheet parameters are public, as
 * 3.4.5.2 and XSLT 3.0 have it), one per overriding declaration of an
 * xsl:override (its visibility attribute, else private, 3.4.5.3), and one
 * per component of a used package that it accepts with a visibility
 * other than hidden (xsl:accept, else public and final become private,
 * 3.4.5.2). Hidden instances are left out: nothing can refer to them, and
 * their own references were bound inside the used package.
 *
 * Reference binding (3.4.5.5): a reference from package P binds to P's
 * instance of the symbolic identifier (kind, name and, for functions, an
 * arity in the arity range); then, while the instance's package is used
 * by another package, to the overriding instance there when one
 * overrides it, else to the accepted copy there, else it stays. The
 * declaration of the last instance is the one called. So a reference
 * inside a used package reaches an override in its user, as 3.4.5.5
 * requires, and two uses of one package (two copies of its modules) are
 * bound independently. xsl:original binds to the overridden instance's
 * declaration (3.4.5.4). Modes are not overridable; the unnamed mode is
 * each package's own.
 *
 * Static errors (3.4): XTSE0020 (a package-version that is no version or
 * range), XTSE3003 (a used package with another name or a version out of
 * range), XTSE3010, XTSE3020, XTSE3022, XTSE3025 (xsl:expose), XTSE3030,
 * XTSE3032, XTSE3040, XTSE3051 (xsl:accept), XTSE3050 (homonymous
 * instances), XTSE3055, XTSE3058, XTSE3060, XTSE3070 (arity ranges; parameter,
 * result, variable and context item types identical; tunnel and required
 * flags) (xsl:override), XTSE3080 (an abstract component in the top-level
 * package), all reported at the element.
 *
 * Thread safety: the lowering's (D35).
 */

/* What a component reference names (3.4.5). */
typedef enum sxml_xslt_component_kind {
    SXML_XSLT_COMPONENT_KIND_NONE = 0,
    SXML_XSLT_COMPONENT_KIND_FUNCTION,
    /* A named template (the name of an xsl:template). */
    SXML_XSLT_COMPONENT_KIND_TEMPLATE,
    SXML_XSLT_COMPONENT_KIND_ATTRIBUTE_SET,
    /* A global variable or stylesheet parameter. */
    SXML_XSLT_COMPONENT_KIND_VARIABLE,
    SXML_XSLT_COMPONENT_KIND_MODE,
} sxml_xslt_component_kind;

/* The visibility of a component instance (3.4.5.1). */
typedef enum sxml_xslt_visibility {
    SXML_XSLT_VISIBILITY_NONE = 0,
    SXML_XSLT_VISIBILITY_PRIVATE,
    SXML_XSLT_VISIBILITY_PUBLIC,
    SXML_XSLT_VISIBILITY_FINAL,
    SXML_XSLT_VISIBILITY_ABSTRACT,
    SXML_XSLT_VISIBILITY_HIDDEN,
} sxml_xslt_visibility;

/* The packages and component instances of a stylesheet (glossary
   `package_table`); src/xslt_package.c. */
typedef struct sxml_xslt_package_table sxml_xslt_package_table;

struct sxml_xslt_lowering;

/*
 * Makes lowering->packages from the sealed AST before anything else is
 * lowered, outside a machine, and checks the rules above. Returns
 * SXML_STATUS_STATIC_ERROR with the error record filled, and the
 * allocator's failures; sxml_xslt_release_packages releases the table
 * (also after a failure).
 */
SXML_NODISCARD sxml_status sxml_xslt_declare_packages(struct sxml_xslt_lowering *lowering);

/* Releases lowering->packages and sets it NULL; NULL is allowed. */
void sxml_xslt_release_packages(struct sxml_xslt_lowering *lowering);

/* The top module (an xsl:package, or the principal module) of node's
   package, from the AST alone: the identity of a package for the parts
   compiled without the package table (output definitions and character
   maps, 3.4.8; D163). Module 1 for node 0. */
sxml_xslt_ast_module_index sxml_xslt_package_top_module(const sxml_xslt_ast *ast,
                                                        sxml_xslt_ast_node_index node);

/* The package of node (1 without a table or for node 0). */
uint32_t sxml_xslt_package_of_node(const struct sxml_xslt_lowering *lowering,
                                   sxml_xslt_ast_node_index node);

/* The package of an AST static context (1 when unknown). */
uint32_t sxml_xslt_package_of_static_context(const struct sxml_xslt_lowering *lowering,
                                             sxml_xslt_ast_static_context_index static_context);

/* The root (xsl:package, or xsl:stylesheet for an implicit package) of
   package's principal module; the principal module's root without a
   table. */
sxml_xslt_ast_node_index sxml_xslt_package_get_root(const struct sxml_xslt_lowering *lowering,
                                                    uint32_t package);

/* The number of packages (1 without a table). */
uint32_t sxml_xslt_package_count(const struct sxml_xslt_lowering *lowering);

/* A reference to bind: a symbolic identifier and the package it is
   written in. */
typedef struct sxml_xslt_component_reference {
    sxml_xslt_component_kind component_kind;
    sxml_name_identifier component_name;
    /* FUNCTION: the arity called. */
    uint32_t arity;
    uint32_t package;
} sxml_xslt_component_reference;

/* What a reference binds to. */
typedef struct sxml_xslt_component_binding {
    /* The declaration whose component is called (for an attribute set,
       one of its xsl:attribute-set declarations; for a mode, its
       xsl:mode); 0 when the package has no such component. */
    sxml_xslt_ast_node_index declaration;
    /* The package that declares it: for a mode, whose mode it is (also
       for an undeclared mode, the referring package). */
    uint32_t package;
    /* It is an abstract component no package overrides (XTDE3052 when
       called, 3.4.5.2). */
    bool is_abstract;
} sxml_xslt_component_binding;

/* Binds reference (3.4.5.5); a binding with declaration 0 when there is
   nothing to bind to (the caller's own error then, such as XPST0017).
   Without a table: declaration 0, package 1. */
sxml_xslt_component_binding sxml_xslt_package_bind(const struct sxml_xslt_lowering *lowering,
                                                   const sxml_xslt_component_reference *reference);

/* The visibility of the instance reference names in its package (before
   any successor; 2.4: the initial template, mode and function are the
   top-level package's public or final ones); NONE when none. */
sxml_xslt_visibility
sxml_xslt_package_get_reference_visibility(const struct sxml_xslt_lowering *lowering,
                                           const sxml_xslt_component_reference *reference);

/* The component xsl:original names inside overriding declaration
   (3.4.5.4): the overridden instance's declaration and package;
   declaration 0 when overriding is no overriding declaration. */
sxml_xslt_component_binding
sxml_xslt_package_bind_original(const struct sxml_xslt_lowering *lowering,
                                sxml_xslt_ast_node_index overriding);

/* The overriding declaration (a child of xsl:override) whose static
   context, or an ancestor of it, is static_context; 0 for none. */
sxml_xslt_ast_node_index
sxml_xslt_package_overriding_declaration_of(const struct sxml_xslt_lowering *lowering,
                                            sxml_xslt_ast_static_context_index static_context);

/* The overriding declaration that holds node (node itself or an
   ancestor); 0 for none. */
sxml_xslt_ast_node_index
sxml_xslt_package_overriding_ancestor_of(const struct sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node);

/* The visibility declaration has in its own package (after xsl:expose;
   3.4.5.1); NONE for a declaration that is no component instance. */
sxml_xslt_visibility sxml_xslt_package_get_visibility(const struct sxml_xslt_lowering *lowering,
                                                      sxml_xslt_ast_node_index declaration);

/* True when declaration's component is abstract (its body raises
   XTDE3052 when called, 3.4.5.2). */
bool sxml_xslt_package_is_abstract(const struct sxml_xslt_lowering *lowering,
                                   sxml_xslt_ast_node_index declaration);

/* True when the two nodes are in one package. */
bool sxml_xslt_package_is_same(const struct sxml_xslt_lowering *lowering,
                               sxml_xslt_ast_node_index first, sxml_xslt_ast_node_index second);

/* Zeroed scratch memory kept until sxml_xslt_release_packages (the
   per-package views other modules make). */
typedef struct sxml_xslt_package_memory_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} sxml_xslt_package_memory_request;

/* SXML_STATUS_INVALID_ARGUMENT without a table; the allocator's
   failures. */
SXML_NODISCARD sxml_status sxml_xslt_package_allocate(const struct sxml_xslt_lowering *lowering,
                                                      sxml_xslt_package_memory_request request,
                                                      void **memory);

/* --------------------------------------------------- Versions (3.4.1) */

/* A version and a version range to compare (borrowed). */
typedef struct sxml_xslt_package_version_query {
    sxml_byte_view package_version;
    sxml_byte_view version_range;
} sxml_xslt_package_version_query;

/* Whether the version matches the range. */
typedef struct sxml_xslt_package_version_match {
    bool matches;
} sxml_xslt_package_version_match;

/* Writes whether package_version matches version_range ("*", lists of
   versions, prefixes "V.*", "V+", "to V", "V to W"; leading and trailing
   whitespace ignored) to *match. SXML_STATUS_INVALID_ARGUMENT when
   either text breaks the grammar (XTSE0020 for the caller). */
SXML_NODISCARD sxml_status sxml_xslt_package_version_matches(
    sxml_xslt_package_version_query query, sxml_xslt_package_version_match *match);
