#pragma once
/*
 * Value templates (XSLT 4.0, section 5.6): attribute value templates
 * (5.6.1) and text value templates (5.6.2). Module xslt_value_template,
 * defined in src/xslt_value_template.c (packet 4.3).
 *
 * A value template is parsed with the tree-sitter grammar
 * grammars/value_template (D97), which inherits grammars/xpath, through
 * the XPath front end's parser part seam (D111). The result is an XPath
 * AST whose XPATH root has a STRING_TEMPLATE body (D102), so it lowers as
 * an XPath 4.0 string template does (`sxml_xpath_lower`, or the embedding
 * entry of src/xpath_lower_internal.h for a stylesheet's static context):
 * fixed parts concatenated with the values of variable parts, each joined
 * with single spaces.
 *
 * For `a{$x}b{{c}}`:
 *
 *   XPATH
 *     body STRING_TEMPLATE
 *       part STRING_TEMPLATE_FIXED_PART "a"
 *       part ENCLOSED_EXPRESSION
 *         expression VARIABLE_REFERENCE ...
 *       part STRING_TEMPLATE_FIXED_PART "b{c}"
 *
 * The same AST as the XPath expression `` `a{$x}b{{c}}` ``, except for the
 * source spans.
 */
#include "sxml/status.h"
#include "xpath_ast_internal.h"

/*
 * Parses request->expression_text, the string value of an attribute that
 * is designated as an attribute value template or of a text node that is
 * a text value template, and writes an AST, owned (release with
 * sxml_ast_destroy), to *ast. The root is an XPATH node whose body is a
 * STRING_TEMPLATE node; its parts, in source order, are
 * STRING_TEMPLATE_FIXED_PART nodes, whose text is the expansion of the
 * fixed part ("{{" and "}}" become single curly brackets), and
 * ENCLOSED_EXPRESSION nodes, one per variable part, whose expression is
 * absent when the variable part holds only whitespace and comments (its
 * expansion is the zero-length string). A zero-length template gives a
 * body without parts. Source spans are offsets into the expression text;
 * the source name, allocator, depth bound and error record are used as
 * by sxml_xpath_build_ast.
 *
 * Returns SXML_STATUS_STATIC_ERROR with the error record filled (the err
 * namespace): XTSE0370 for an unescaped "}" in a fixed part, XTSE0350 for
 * a "{" without a matching "}", XPST0003 when a variable part is not
 * XPath 4.0 Expr? (5.6); other XPath static errors are raised when the
 * AST is lowered. Also SXML_STATUS_INVALID_UTF8,
 * SXML_STATUS_LIMIT_EXCEEDED, SXML_STATUS_INVALID_ARGUMENT for a NULL
 * request or ast, and the failures of the allocator. *ast is untouched on
 * failure.
 */
SXML_NODISCARD sxml_status sxml_xslt_build_value_template_ast(
    const sxml_ast_build_request *request, sxml_ast **ast);
