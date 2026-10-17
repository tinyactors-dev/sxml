#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"

/*
 * The stylesheet AST of XSLT 4.0 (docs/DECISIONS.md, D24, D52, D102).
 * Internal to sxml.
 *
 * A stylesheet module is an XML document: the processor's XML parser reads
 * it into an arena tree, and the stylesheet reader (packet 4.5,
 * src/xslt_read.c) walks that tree and builds one of these for the whole
 * stylesheet: every module it includes and imports, with one node per
 * stylesheet element and per text node that survives stylesheet
 * whitespace stripping. XSLT lowering (packets 4.6 to 4.14, the XSLT
 * lowering parts of src/xpath_lower_internal.h) reads the AST and writes
 * IR (src/ir_internal.h); the AST is never evaluated. A used package
 * (xsl:use-package, packet 4.16) is an AST of its own.
 *
 * What the reader has done before a node exists
 * ----------------------------------------------
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, as of October 2026).
 * - "Stripping Whitespace and Commentary from the Stylesheet" (3.11.1):
 *   comments, processing instructions, xsl:note elements and whitespace
 *   text nodes are gone, except whitespace text in xsl:text and under
 *   xml:space="preserve"; adjacent text is one TEXT_NODE.
 * - "Conditional Element Inclusion" (3.11.3): an element whose
 *   [xsl:]use-when is false, with its subtree, has no node.
 * - "Shadow Attributes" (3.11.4): a shadow attribute `_name` was evaluated
 *   and replaced the attribute `name` (flag SHADOW).
 * - "Simplified Stylesheet Modules" (3.7): a module whose outermost
 *   element is not xsl:stylesheet, xsl:transform or xsl:package gets a
 *   synthesized STYLESHEET root holding a synthesized TEMPLATE with match
 *   "." (a pattern parsed from that text), and the outermost element is
 *   the template's only child; version is [xsl:]version or 4.0, and
 *   expand-text is "yes" unless that version is 3.0 or less.
 * - Attributes are checked against the element syntax of the
 *   specification (XTSE0010, XTSE0090, "Forwards Compatible Processing"
 *   3.9: unknown attributes and elements in forwards-compatible mode are
 *   dropped or become UNKNOWN_INSTRUCTION) and each value is read into the
 *   form its syntax names (see sxml_xslt_ast_attribute_kind); every
 *   expression, pattern, value template and type is parsed into an
 *   sxml_ast of its own by the front ends (XPath 1.16, patterns 4.4, value
 *   templates 4.3), so syntax errors (XPST0003, XTSE0340, XTSE0350) are
 *   raised while reading.
 * - QNames in attributes (EQName, the name lists) are resolved against the
 *   element's static context into name pool identifiers (XTSE0280); names
 *   inside expressions, patterns and types stay as written in their
 *   sxml_ast and are resolved by static analysis with the static context
 *   of their expression (sxml_xslt_ast_get_namespace_bindings).
 * - xsl:include and xsl:import are followed through the resource resolver
 *   (XSLT_MODULE kind) and their modules read in place, depth first
 *   (XTSE0165 for a module that cannot be read, XTSE0210 for a cycle
 *   through an xsl:import). An xsl:include of a module already included
 *   in its stylesheet level, or of the level's top module, is ignored
 *   (3.10.2): its INCLUDE node has included_module 0.
 *
 * What lowering still does: everything the specification decides from the
 * whole stylesheet (duplicate declarations, XTSE0630 and friends, the
 * order of template rules, variable references, types of parameters).
 *
 * Modules and import precedence (3.10)
 * ------------------------------------
 * A module is one stylesheet module as reached by one xsl:include or
 * xsl:import, or the principal module (module 1). The same document met
 * twice is two modules sharing one tree (each import has its own
 * precedence, "Stylesheet Import" 3.10.3). Modules are added in pre-order
 * of the module tree: a module's parent is the module last added or one of
 * its ancestors. sxml_xslt_ast_seal then numbers the modules so that an
 * importing module has higher precedence than everything it imports, and
 * a later import higher than an earlier one: the post-order position of
 * each module in the import tree, from 1 (lowest); an included module
 * takes the precedence of the module that includes it ("Stylesheet
 * Inclusion" 3.10.2). Precedence values are ordered, not dense.
 *
 * Declarations (3.6)
 * ------------------
 * Every child of a module's root is a declaration (flag DECLARATION) and
 * is listed in the declaration list, in declaration order: the order of
 * the stylesheet with each included module's declarations in place of its
 * xsl:include, and each imported module's in place of its xsl:import.
 * Lowering iterates that list (sxml_xslt_ast_get_declaration) instead of
 * walking modules; a node's module gives its import precedence. A template
 * inside xsl:mode ("Enclosing Modes" 6.9.6) is a child of the MODE node,
 * not a declaration of its own.
 *
 * The static context of an element (3.3, 3.6, 5.1)
 * -------------------------------------------------
 * Each node points to an sxml_xslt_ast_static_context that holds what the
 * standard attributes and namespace declarations of the node and its
 * ancestors (within the module) decide, fully resolved, so lowering never
 * walks upwards: the statically known namespaces for expressions (after
 * fixed-namespaces, 3.6.1), [xsl:]xpath-default-namespace, the default
 * collation, default mode and default validation, expand-text, the
 * version, the excluded and extension namespaces (cumulative, 11.1.3,
 * 24.3) and the static base URI (xml:base). A node shares its parent's
 * static context unless one of these changes; the root of a module has
 * one whose parent is 0. The standard attributes themselves stay on the
 * node as TEXT attributes, which lowering does not read.
 *
 * Storage (D24, as the IR unit)
 * ----------------------------
 * Nodes, attributes, expressions, static contexts, modules, name tokens,
 * namespace bindings, namespace URIs and text live in arrays addressed by
 * index, with entry 0 reserved and all zero ("none"). The arrays grow
 * through the AST's allocator while the reader builds, so pointers and
 * views into them are valid only until the next add; after
 * sxml_xslt_ast_seal nothing grows and they are valid for the AST's
 * lifetime. Module URIs are allocated once each and never move, so the
 * sxml_ast of every expression borrows its module's URI as source name.
 * The AST owns the sxml_ast of every expression and a reference to the
 * document node of every module (sxml_item, D33); finalize releases them.
 *
 * Text form
 * ---------
 * sxml_xslt_ast_write_text prints an AST for golden tests: modules, static
 * contexts and the declaration list, then each module's nodes from its
 * root in document order, one per line, indented two spaces per level,
 * each with its attributes on the lines below and expressions as their
 * kind, number and text. A node's static context is printed where it
 * differs from its parent's. Expression text is the attribute value as
 * written; positions are line:column.
 *
 * Example 1: a simplified stylesheet (QT4 XSLT for-each-group-001.xsl)
 * --------------------------------------------------------------------
 *
 *      1  <?spec xslt#grouping?>
 *      2  <table xsl:version="2.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
 *      3  <!-- simple test of value-based grouping -->
 *      4    <tr>
 *      5      <th>Position</th>
 *      6      <th>Country</th>
 *      7      <th>City List</th>
 *      8      <th>Population</th>
 *      9    </tr>
 *     10    <xsl:for-each-group select="cities/city" group-by="@country">
 *     11      <tr>
 *     12        <td><xsl:value-of select="position()"/></td>
 *     13        <td><xsl:value-of select="@country"/></td>
 *     14        <td>
 *     15          <xsl:for-each select="current-group()">
 *     16            <xsl:value-of select="@name"/>
 *     17            <xsl:if test="position() != last()">, </xsl:if>
 *     18          </xsl:for-each>
 *     19        </td>
 *     20        <td><xsl:value-of select="sum(current-group()/@pop)"/></td>
 *     21      </tr>
 *     22    </xsl:for-each-group>
 *     23  </table>
 *
 * The processing instruction, the comment and the whitespace text nodes
 * are stripped; the outermost element is a literal result element, so the
 * module is simplified:
 *
 *     module 1 principal simplified precedence 1 root 1
 *       uri "for-each-group-001.xsl"
 *     context 1 parent 0 version 2.0 expand-text no
 *       namespaces (xsl = http://www.w3.org/1999/XSL/Transform)
 *       xpath-default-namespace none default-collation codepoint
 *       default-mode #unnamed default-validation strip
 *       excluded (http://www.w3.org/1999/XSL/Transform) extension ()
 *       base-uri "for-each-group-001.xsl"
 *     declarations (node 2)
 *     node 1 STYLESHEET module 1 synthesized context 1 at 2:1
 *       node 2 TEMPLATE synthesized declaration 1 at 2:1
 *         attribute MATCH synthesized pattern 1 "."
 *         node 3 LITERAL_RESULT_ELEMENT table at 2:1
 *           attribute VERSION xslt-namespace text "2.0" at 2:8
 *           node 4 LITERAL_RESULT_ELEMENT tr at 4:3
 *             node 5 LITERAL_RESULT_ELEMENT th at 5:5
 *               node 6 TEXT_NODE text "Position" at 5:9
 *             node 7 LITERAL_RESULT_ELEMENT th at 6:5
 *               node 8 TEXT_NODE text "Country" at 6:9
 *             node 9 LITERAL_RESULT_ELEMENT th at 7:5
 *               node 10 TEXT_NODE text "City List" at 7:9
 *             node 11 LITERAL_RESULT_ELEMENT th at 8:5
 *               node 12 TEXT_NODE text "Population" at 8:9
 *           node 13 FOR_EACH_GROUP at 10:3
 *             attribute SELECT expression 2 "cities/city" at 10:23
 *             attribute GROUP_BY expression 3 "@country" at 10:44
 *             node 14 LITERAL_RESULT_ELEMENT tr at 11:5
 *               node 15 LITERAL_RESULT_ELEMENT td at 12:7
 *                 node 16 VALUE_OF at 12:11
 *                   attribute SELECT expression 4 "position()" at 12:25
 *               node 17 LITERAL_RESULT_ELEMENT td at 13:7
 *                 node 18 VALUE_OF at 13:11
 *                   attribute SELECT expression 5 "@country" at 13:25
 *               node 19 LITERAL_RESULT_ELEMENT td at 14:7
 *                 node 20 FOR_EACH at 15:9
 *                   attribute SELECT expression 6 "current-group()" at 15:23
 *                   node 21 VALUE_OF at 16:11
 *                     attribute SELECT expression 7 "@name" at 16:25
 *                   node 22 IF at 17:11
 *                     attribute TEST expression 8 "position() != last()"
 *                       at 17:19
 *                     node 23 TEXT_NODE text ", " at 17:47
 *               node 24 LITERAL_RESULT_ELEMENT td at 20:7
 *                 node 25 VALUE_OF at 20:11
 *                   attribute SELECT expression 9
 *                     "sum(current-group()/@pop)" at 20:25
 *
 * Every node has static context 1: the synthesized nodes take that of the
 * outermost element, which holds its namespace declaration and
 * xsl:version, and no element below declares more. Expression 1 is a pattern AST
 * (root PATTERN, packet 4.4) of the text "."; expressions 2 to 9 are XPath
 * ASTs whose root is XPATH, for example expression 2:
 *
 *     XPATH
 *       body: RELATIVE_PATH_EXPRESSION operator SLASH
 *         left: AXIS_STEP axis NONE  test: NCNAME "cities"
 *         right: AXIS_STEP axis NONE  test: NCNAME "city"
 *
 * Lowering (4.6, 4.8, 4.9) turns node 2 into template rule 1 of the
 * unnamed mode (precedence 1, priority -1 for the pattern ".", 6.4.2),
 * node 3 into an ELEMENT_CONSTRUCTOR without a namespace node for the
 * excluded XSLT namespace, and node 13 into the grouping of packet 4.9.
 *
 * Example 2: a template rule and a stylesheet function, with an import
 * -------------------------------------------------------------------
 *
 *   main.xsl:
 *      1  <xsl:stylesheet version="4.0" expand-text="yes"
 *      2      xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
 *      3      xmlns:xs="http://www.w3.org/2001/XMLSchema"
 *      4      xmlns:f="urn:example:f" exclude-result-prefixes="xs f">
 *      5    <xsl:import href="base.xsl"/>
 *      6    <xsl:template match="item[@code]" mode="list" priority="2">
 *      7      <xsl:param name="depth" as="xs:integer" select="0"/>
 *      8      <li class="{@code}">{f:label(., $depth)}</li>
 *      9    </xsl:template>
 *     10    <xsl:function name="f:label" as="xs:string">
 *     11      <xsl:param name="item" as="element(item)"/>
 *     12      <xsl:param name="depth" as="xs:integer"/>
 *     13      <xsl:sequence select="string-join(($item/@code, $depth), '-')"/>
 *     14    </xsl:function>
 *     15  </xsl:stylesheet>
 *
 *   base.xsl:
 *      1  <xsl:stylesheet version="3.0"
 *      2      xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
 *      3    <xsl:template match="item" mode="list"/>
 *      4  </xsl:stylesheet>
 *
 * The reader meets xsl:import first, reads base.xsl as module 2 at once
 * and then continues with main.xsl:
 *
 *     module 1 principal precedence 2 root 1 uri "main.xsl"
 *     module 2 imported parent 1 by node 2 precedence 1 root 3
 *       uri "base.xsl"
 *     context 1 parent 0 version 4.0 expand-text yes
 *       namespaces (xsl = http://www.w3.org/1999/XSL/Transform,
 *         xs = http://www.w3.org/2001/XMLSchema, f = urn:example:f)
 *       xpath-default-namespace none default-collation codepoint
 *       default-mode #unnamed default-validation strip
 *       excluded (http://www.w3.org/1999/XSL/Transform,
 *         http://www.w3.org/2001/XMLSchema, urn:example:f) extension ()
 *       base-uri "main.xsl"
 *     context 2 parent 0 version 3.0 expand-text no
 *       namespaces (xsl = http://www.w3.org/1999/XSL/Transform)
 *       xpath-default-namespace none default-collation codepoint
 *       default-mode #unnamed default-validation strip
 *       excluded (http://www.w3.org/1999/XSL/Transform) extension ()
 *       base-uri "base.xsl"
 *     declarations (node 2, node 4, node 5, node 9)
 *     node 1 STYLESHEET module 1 context 1 at 1:1
 *       attribute VERSION text "4.0" at 1:17
 *       attribute EXPAND_TEXT text "yes" at 1:31
 *       attribute EXCLUDE_RESULT_PREFIXES text "xs f" at 4:29
 *       node 2 IMPORT declaration 1 imports module 2 at 5:3
 *         attribute HREF text "base.xsl" at 5:15
 *       node 5 TEMPLATE declaration 3 at 6:3
 *         attribute MATCH pattern 2 "item[@code]" at 6:17
 *         attribute MODE names (list) at 6:37
 *         attribute PRIORITY number 2 at 6:49
 *         node 6 PARAM at 7:5
 *           attribute NAME name depth at 7:16
 *           attribute AS sequence-type 3 "xs:integer" at 7:29
 *           attribute SELECT expression 4 "0" at 7:45
 *         node 7 LITERAL_RESULT_ELEMENT li at 8:5
 *           attribute LITERAL_RESULT class value-template 5 "{@code}"
 *             at 8:9
 *           node 8 TEXT_NODE value-template 6 "{f:label(., $depth)}"
 *             at 8:25
 *       node 9 FUNCTION declaration 4 at 10:3
 *         attribute NAME name Q{urn:example:f}label at 10:17
 *         attribute AS sequence-type 7 "xs:string" at 10:32
 *         node 10 PARAM at 11:5
 *           attribute NAME name item at 11:16
 *           attribute AS sequence-type 8 "element(item)" at 11:28
 *         node 11 PARAM at 12:5
 *           attribute NAME name depth at 12:16
 *           attribute AS sequence-type 9 "xs:integer" at 12:29
 *         node 12 SEQUENCE at 13:5
 *           attribute SELECT expression 10
 *             "string-join(($item/@code, $depth), '-')" at 13:19
 *     node 3 STYLESHEET module 2 context 2 at 1:1
 *       attribute VERSION text "3.0" at 1:17
 *       node 4 TEMPLATE declaration 2 at 3:3
 *         attribute MATCH pattern 1 "item" at 3:17
 *         attribute MODE names (list) at 3:30
 *
 * Nodes 3 and 4 are numbered before node 5 because the reader read
 * base.xsl when it met node 2; the text form prints each module's tree
 * under its root. Expressions 5 and 6 are value templates: XPath ASTs
 * whose root is XPATH and whose body is a STRING_TEMPLATE (fixed parts and
 * enclosed expressions, packet 4.3), so they lower as string templates do
 * ("Value Templates" 5.6). Sequence types 3, 7, 8 and 9 are ASTs whose root
 * is SEQUENCE_TYPE (sxml_xpath_build_sequence_type_ast).
 *
 * Lowering then gives (src/ir_internal.h, "Lowering XSLT 4.0"):
 * - node 5: mode "list" gets two template rules, {match function of
 *   pattern 2, template function of node 5's children, priority 2,
 *   precedence 2, declaration order 3} and {pattern 1, an empty template
 *   function, priority 0 (default for "item", 6.4), precedence 1,
 *   declaration order 2}; node 5 is tried first (higher precedence). Its
 *   template function has the focus in slots 1 to 3 and parameter $depth
 *   (declared type xs:integer, default 0) in slot 4;
 * - node 7: ELEMENT_CONSTRUCTOR li with no namespace nodes (xsl, xs and f
 *   are excluded), an attribute constructor for class from value template
 *   5, and a text constructor from value template 6;
 * - node 9: an IR function Q{urn:example:f}label#2, with parameters 1 and
 *   2 coerced to element(item) and xs:integer and the result to xs:string,
 *   entered in the function table that static analysis of every expression
 *   of the stylesheet sees (expression 6 calls it).
 *
 * Thread safety: an AST is used by one thread at a time, the thread of its
 * item heap (D35).
 */

/* ----------------------------------------------------------- Identifiers */

/* A node of a stylesheet AST; 0 means none. */
typedef struct sxml_xslt_ast_node_index {
    uint32_t value;
} sxml_xslt_ast_node_index;

/* An attribute of a node; 0 means none. */
typedef struct sxml_xslt_ast_attribute_index {
    uint32_t value;
} sxml_xslt_ast_attribute_index;

/* An expression, pattern, value template or type of the AST; 0 means
   none. */
typedef struct sxml_xslt_ast_expression_index {
    uint32_t value;
} sxml_xslt_ast_expression_index;

/* A static context; 0 means none. */
typedef struct sxml_xslt_ast_static_context_index {
    uint32_t value;
} sxml_xslt_ast_static_context_index;

/* A stylesheet module; 0 means none, 1 is the principal module. */
typedef struct sxml_xslt_ast_module_index {
    uint32_t value;
} sxml_xslt_ast_module_index;

/* Bytes in the text store: byte_count bytes from byte_offset. */
typedef struct sxml_xslt_ast_text {
    uint32_t byte_offset;
    uint32_t byte_count;
} sxml_xslt_ast_text;

/* A run of attributes: those of one node, always consecutive. */
typedef struct sxml_xslt_ast_attribute_list {
    uint32_t first_attribute_index;
    uint32_t attribute_count;
} sxml_xslt_ast_attribute_list;

/* A run of name tokens (the value of a NAME_LIST attribute). */
typedef struct sxml_xslt_ast_name_token_list {
    uint32_t first_name_token_index;
    uint32_t name_token_count;
} sxml_xslt_ast_name_token_list;

/* A run of namespace bindings. */
typedef struct sxml_xslt_ast_namespace_binding_list {
    uint32_t first_namespace_binding_index;
    uint32_t namespace_binding_count;
} sxml_xslt_ast_namespace_binding_list;

/* A run of namespace URIs. */
typedef struct sxml_xslt_ast_namespace_uri_list {
    uint32_t first_namespace_uri_index;
    uint32_t namespace_uri_count;
} sxml_xslt_ast_namespace_uri_list;

/* ----------------------------------------------------------------- Kinds */

/*
 * The kind of a node: one per element of the XSLT namespace that can
 * reach the AST (xsl:transform is STYLESHEET, xsl:note never arrives),
 * named after the element (xsl:apply-templates is APPLY_TEMPLATES), and
 * five kinds for everything else. The elements and their attributes are
 * those of the XSLT 4.0 schema (schema-for-xslt40.xsd, appendix "Schema
 * for XSLT Stylesheets"); the element syntax summaries give the content.
 * Values are only ever appended.
 */
typedef enum sxml_xslt_ast_kind {
    SXML_XSLT_AST_KIND_NONE = 0,

    /* --- Not XSLT elements --- */
    /* An element outside the XSLT namespace in a sequence constructor
       ("Literal Result Elements" 11.1). element_name and namespace_prefix
       give its name; attributes are LITERAL_RESULT value templates and
       the XSLT-namespaced ones (flag XSLT_NAMESPACE); children are its
       sequence constructor. */
    SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT,
    /* A text node of a sequence constructor or of xsl:text ("Literal
       Text Nodes" 11.4.1). content is TEXT, or VALUE_TEMPLATE when
       expand-text is yes ("Text Value Templates" 5.6.2). */
    SXML_XSLT_AST_KIND_TEXT_NODE,
    /* An element in an extension namespace ("Extension Instructions"
       24.3); sxml knows none, so lowering runs its xsl:fallback children
       or raises XTDE1450. Attributes are FOREIGN. */
    SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION,
    /* An element of the XSLT namespace that sxml does not know, read in
       forwards-compatible mode ("Forwards Compatible Processing" 3.9):
       as EXTENSION_INSTRUCTION. Outside that mode it is XTSE0010. */
    SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION,
    /* A top-level element in a non-null namespace other than the XSLT
       namespace ("User-defined Data Elements" 3.6.4): a declaration with
       no attributes or children in the AST; source_node reaches it. */
    SXML_XSLT_AST_KIND_DATA_ELEMENT,

    /* --- Module roots (3.4, 3.6) --- */
    /* xsl:stylesheet or xsl:transform, or the synthesized root of a
       simplified module. Children: declarations. */
    SXML_XSLT_AST_KIND_STYLESHEET,
    /* xsl:package. Children: xsl:expose and declarations. */
    SXML_XSLT_AST_KIND_PACKAGE,

    /* --- Declarations (children of a module root; PARAM, VARIABLE and
       TEMPLATE also elsewhere, as noted) --- */
    SXML_XSLT_AST_KIND_ACCUMULATOR,     /* 19.1; children ACCUMULATOR_RULE */
    SXML_XSLT_AST_KIND_ATTRIBUTE_SET,   /* 10.2; children ATTRIBUTE */
    SXML_XSLT_AST_KIND_CHARACTER_MAP,   /* 26.1.6; OUTPUT_CHARACTER */
    SXML_XSLT_AST_KIND_DECIMAL_FORMAT,  /* 5.5 */
    /* 10.3 "Stylesheet Functions": PARAM children, then the body. */
    SXML_XSLT_AST_KIND_FUNCTION,
    SXML_XSLT_AST_KIND_GLOBAL_CONTEXT_ITEM, /* 3.4.9 */
    /* 3.10.3 "Stylesheet Import"; included_module is the module read. */
    SXML_XSLT_AST_KIND_IMPORT,
    /* 3.13; XTSE1660 when sxml reads it (D97: no schema awareness). */
    SXML_XSLT_AST_KIND_IMPORT_SCHEMA,
    /* 3.10.2 "Stylesheet Inclusion"; included_module is the module
       read, 0 for an ignored duplicate. */
    SXML_XSLT_AST_KIND_INCLUDE,
    SXML_XSLT_AST_KIND_ITEM_TYPE,       /* 5.4.1 */
    SXML_XSLT_AST_KIND_KEY,             /* 20.2.1 */
    /* 6.9.1 "Declaring Modes"; children TEMPLATE (6.9.6). */
    SXML_XSLT_AST_KIND_MODE,
    SXML_XSLT_AST_KIND_NAMESPACE_ALIAS, /* 11.1.4 */
    SXML_XSLT_AST_KIND_OUTPUT,          /* 26.1.1 */
    /* 9.2: a stylesheet parameter at top level; also a parameter of
       TEMPLATE, FUNCTION and ITERATE (their first children). */
    SXML_XSLT_AST_KIND_PARAM,
    SXML_XSLT_AST_KIND_PRESERVE_SPACE,  /* 4.2.2 */
    SXML_XSLT_AST_KIND_RECORD_TYPE,     /* 5.4.2; children FIELD */
    SXML_XSLT_AST_KIND_STRIP_SPACE,     /* 4.2.2 */
    /* 6.1 "Defining Templates": CONTEXT_ITEM and PARAM children, then
       the body. Also a child of MODE. */
    SXML_XSLT_AST_KIND_TEMPLATE,
    /* 3.4.2; children ACCEPT, OVERRIDE, PACKAGE_LOCATION. */
    SXML_XSLT_AST_KIND_USE_PACKAGE,
    /* 9.1: a global variable at top level; also an instruction. */
    SXML_XSLT_AST_KIND_VARIABLE,

    /* --- Instructions (elements of a sequence constructor, 5.7) --- */
    /* 17.1; children MATCHING_SUBSTRING, NON_MATCHING_SUBSTRING,
       FALLBACK. */
    SXML_XSLT_AST_KIND_ANALYZE_STRING,
    SXML_XSLT_AST_KIND_APPLY_IMPORTS,   /* 6.11; children WITH_PARAM */
    /* 6.6; children SORT and WITH_PARAM. */
    SXML_XSLT_AST_KIND_APPLY_TEMPLATES,
    SXML_XSLT_AST_KIND_ARRAY,           /* 22.1 */
    SXML_XSLT_AST_KIND_ARRAY_MEMBER,    /* 22.1 */
    SXML_XSLT_AST_KIND_ASSERT,          /* 23.2 */
    SXML_XSLT_AST_KIND_ATTRIBUTE,       /* 11.3 */
    SXML_XSLT_AST_KIND_BREAK,           /* 7.2 */
    SXML_XSLT_AST_KIND_CALL_TEMPLATE,   /* 10.1.2; children WITH_PARAM */
    SXML_XSLT_AST_KIND_CHOOSE,          /* 8.2; WHEN+, OTHERWISE? */
    SXML_XSLT_AST_KIND_COMMENT,         /* 11.8 */
    SXML_XSLT_AST_KIND_COPY,            /* 11.9.1 */
    SXML_XSLT_AST_KIND_COPY_OF,         /* 11.9.2 */
    SXML_XSLT_AST_KIND_DOCUMENT,        /* 11.5 */
    SXML_XSLT_AST_KIND_ELEMENT,         /* 11.2 */
    SXML_XSLT_AST_KIND_EVALUATE,        /* 10.5; children WITH_PARAM */
    SXML_XSLT_AST_KIND_FALLBACK,        /* 24.3.3 */
    SXML_XSLT_AST_KIND_FOR_EACH,        /* 7.1; SORT children first */
    SXML_XSLT_AST_KIND_FOR_EACH_GROUP,  /* 14.1; SORT children first */
    SXML_XSLT_AST_KIND_FORK,            /* 16.1 */
    SXML_XSLT_AST_KIND_IF,              /* 8.1 */
    /* 7.2; PARAM children first, then ON_COMPLETION, then the body. */
    SXML_XSLT_AST_KIND_ITERATE,
    SXML_XSLT_AST_KIND_MAP,             /* 21.1.1 */
    SXML_XSLT_AST_KIND_MAP_ENTRY,       /* 21.1.1 */
    /* 15.2; children MERGE_SOURCE+, MERGE_ACTION. */
    SXML_XSLT_AST_KIND_MERGE,
    SXML_XSLT_AST_KIND_MESSAGE,         /* 23.1 */
    SXML_XSLT_AST_KIND_NAMESPACE,       /* 11.7 */
    SXML_XSLT_AST_KIND_NEXT_ITERATION,  /* 7.2; children WITH_PARAM */
    SXML_XSLT_AST_KIND_NEXT_MATCH,      /* 6.11; children WITH_PARAM */
    SXML_XSLT_AST_KIND_NUMBER,          /* 12 */
    SXML_XSLT_AST_KIND_ON_EMPTY,        /* 8.5.2 */
    SXML_XSLT_AST_KIND_ON_NON_EMPTY,    /* 8.5.3 */
    SXML_XSLT_AST_KIND_PERFORM_SORT,    /* 13.2; SORT children first */
    SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION, /* 11.6 */
    SXML_XSLT_AST_KIND_RESULT_DOCUMENT, /* 26.2 */
    /* 10.4.2: the element's text is an XPath expression, read into
       content (EXPRESSION); the node has no children. */
    SXML_XSLT_AST_KIND_SELECT,
    SXML_XSLT_AST_KIND_SEQUENCE,        /* 10.4.1 */
    SXML_XSLT_AST_KIND_SOURCE_DOCUMENT, /* 18 */
    SXML_XSLT_AST_KIND_SWITCH,          /* 8.3; WHEN+, OTHERWISE? */
    /* 11.4.2; one TEXT_NODE child, or none for an empty xsl:text. */
    SXML_XSLT_AST_KIND_TEXT,
    SXML_XSLT_AST_KIND_TRY,             /* 8.4; children CATCH */
    SXML_XSLT_AST_KIND_VALUE_OF,        /* 11.4.2 */
    SXML_XSLT_AST_KIND_WHERE_POPULATED, /* 8.5.1 */

    /* --- XSLT elements that occur only inside particular parents --- */
    SXML_XSLT_AST_KIND_ACCEPT,          /* 3.4.5.2, in USE_PACKAGE */
    SXML_XSLT_AST_KIND_ACCUMULATOR_RULE, /* 19.1, in ACCUMULATOR */
    SXML_XSLT_AST_KIND_CATCH,           /* 8.4, in TRY */
    SXML_XSLT_AST_KIND_CONTEXT_ITEM,    /* 10.1.4, in TEMPLATE */
    SXML_XSLT_AST_KIND_EXPOSE,          /* 3.4.5, in PACKAGE */
    SXML_XSLT_AST_KIND_FIELD,           /* 5.4.2.1, in RECORD_TYPE */
    SXML_XSLT_AST_KIND_MATCHING_SUBSTRING,     /* 17.1 */
    SXML_XSLT_AST_KIND_MERGE_ACTION,    /* 15.6, in MERGE */
    SXML_XSLT_AST_KIND_MERGE_KEY,       /* 15.4, in MERGE_SOURCE */
    SXML_XSLT_AST_KIND_MERGE_SOURCE,    /* 15.3, in MERGE */
    SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING, /* 17.1 */
    SXML_XSLT_AST_KIND_ON_COMPLETION,   /* 7.2, in ITERATE */
    SXML_XSLT_AST_KIND_OTHERWISE,       /* 8.2, 8.3 */
    SXML_XSLT_AST_KIND_OUTPUT_CHARACTER, /* 26.1.6, in CHARACTER_MAP */
    SXML_XSLT_AST_KIND_OVERRIDE,        /* 3.4.5.3, in USE_PACKAGE */
    SXML_XSLT_AST_KIND_PACKAGE_LOCATION, /* 3.4.3, in USE_PACKAGE */
    SXML_XSLT_AST_KIND_SORT,            /* 13.1 */
    SXML_XSLT_AST_KIND_WHEN,            /* 8.2, 8.3 */
    SXML_XSLT_AST_KIND_WITH_PARAM,      /* 9.10 */

    /* One more than the largest kind. */
    SXML_XSLT_AST_KIND_COUNT,
} sxml_xslt_ast_kind;

/*
 * What an attribute holds once read (sxml_xslt_ast_value). The reader
 * picks the form from the type the element syntax gives the attribute
 * (XSLT 4.0, 2.3 "Notation"); see sxml_xslt_ast_attribute_kind.
 */
typedef enum sxml_xslt_ast_value_kind {
    SXML_XSLT_AST_VALUE_KIND_NONE = 0,
    /* "expression": an XPath AST (root XPATH). */
    SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
    /* "pattern": a pattern AST (packet 4.4). */
    SXML_XSLT_AST_VALUE_KIND_PATTERN,
    /* "{ ... }": an attribute or text value template, an XPath AST whose
       root is XPATH with a STRING_TEMPLATE body (packet 4.3). Always this
       form, also when the value has no enclosed expression: lowering
       folds a template of fixed parts only. */
    SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE,
    /* "sequence-type": an AST whose root is SEQUENCE_TYPE. */
    SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE,
    /* "item-type": as SEQUENCE_TYPE with occurrence EXACTLY_ONE (an
       occurrence indicator is XTSE0020). */
    SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE,
    /* Strings, characters, URIs, IDs, tokens without a fixed list: the
       value as written, whitespace-trimmed where the attribute's type is
       a token type (xs:anyURI, xs:NMTOKEN, xs:ID). URIs are not resolved:
       the node's static base URI is in its static context. */
    SXML_XSLT_AST_VALUE_KIND_TEXT,
    /* "boolean": yes, true, 1 (true) or no, false, 0 (false), 2.3. */
    SXML_XSLT_AST_VALUE_KIND_BOOLEAN,
    /* "decimal": priority, html-version, version of a module root. */
    SXML_XSLT_AST_VALUE_KIND_NUMBER,
    /* One word of a fixed list (sxml_xslt_ast_keyword). */
    SXML_XSLT_AST_VALUE_KIND_KEYWORD,
    /* "eqname", "ncname" (as a name in no namespace): resolved. */
    SXML_XSLT_AST_VALUE_KIND_NAME,
    /* "eqnames", "tokens" of name tests, modes, accumulator names:
       sxml_xslt_ast_name_token entries, resolved. */
    SXML_XSLT_AST_VALUE_KIND_NAME_LIST,
    /* "prefix" or "#default" of xsl:namespace-alias: the prefix with the
       namespace URI it is bound to among the element's in-scope
       namespaces (XTSE0812). */
    SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING,
} sxml_xslt_ast_value_kind;

/*
 * The attributes of XSLT elements, named after the attribute
 * (group-by is GROUP_BY), with the value kind the reader gives each; where
 * the type differs between elements, the exceptions follow ("VALUE_TEMPLATE
 * on result-document"). An attribute named here that the schema does not
 * allow on the element is XTSE0090 (outside forwards-compatible mode).
 * Values are only ever appended.
 */
typedef enum sxml_xslt_ast_attribute_kind {
    SXML_XSLT_AST_ATTRIBUTE_NONE = 0,
    /* An attribute of a literal result element outside the XSLT
       namespace: VALUE_TEMPLATE ("Attribute Value Templates" 5.6.1). */
    SXML_XSLT_AST_ATTRIBUTE_LITERAL_RESULT,
    /* An attribute in a namespace other than XSLT's on an XSLT element
       (allowed, 3.3), or any attribute of an extension instruction or
       unknown instruction: TEXT, ignored by lowering. */
    SXML_XSLT_AST_ATTRIBUTE_FOREIGN,

    /* Standard attributes (3.3) and fixed-namespaces (3.6.1): TEXT; their
       effect is in the static context. Flag XSLT_NAMESPACE when written
       xsl:version and so on (on a literal result element). */
    SXML_XSLT_AST_ATTRIBUTE_DEFAULT_COLLATION,
    SXML_XSLT_AST_ATTRIBUTE_DEFAULT_MODE,
    SXML_XSLT_AST_ATTRIBUTE_DEFAULT_VALIDATION,
    SXML_XSLT_AST_ATTRIBUTE_EXCLUDE_RESULT_PREFIXES,
    SXML_XSLT_AST_ATTRIBUTE_EXPAND_TEXT,
    SXML_XSLT_AST_ATTRIBUTE_EXTENSION_ELEMENT_PREFIXES,
    SXML_XSLT_AST_ATTRIBUTE_FIXED_NAMESPACES,
    SXML_XSLT_AST_ATTRIBUTE_SCHEMA_ROLE,
    SXML_XSLT_AST_ATTRIBUTE_USE_WHEN,
    /* Also xsl:output's version, the serialization parameter. */
    SXML_XSLT_AST_ATTRIBUTE_VERSION,
    SXML_XSLT_AST_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE,

    /* allow-duplicate-names: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_ALLOW_DUPLICATE_NAMES,
    /* archive-type: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_ARCHIVE_TYPE,
    /* as: SEQUENCE_TYPE; ITEM_TYPE on context-item, global-context-item,
       item-type. */
    SXML_XSLT_AST_ATTRIBUTE_AS,
    /* base-uri: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_BASE_URI,
    /* build-tree: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_BUILD_TREE,
    /* byte-order-mark: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_BYTE_ORDER_MARK,
    /* cache: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_CACHE,
    /* canonical: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_CANONICAL,
    /* capture: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_CAPTURE,
    /* case-order: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_CASE_ORDER,
    /* cdata: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_CDATA,
    /* cdata-section-elements: NAME_LIST; VALUE_TEMPLATE on
       result-document. */
    SXML_XSLT_AST_ATTRIBUTE_CDATA_SECTION_ELEMENTS,
    /* character: TEXT (one character, XTSE0020 otherwise). */
    SXML_XSLT_AST_ATTRIBUTE_CHARACTER,
    /* collation: VALUE_TEMPLATE; TEXT on key. */
    SXML_XSLT_AST_ATTRIBUTE_COLLATION,
    /* compiled-regex: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_COMPILED_REGEX,
    /* component: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_COMPONENT,
    /* composite: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_COMPOSITE,
    /* context-item: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_CONTEXT_ITEM,
    /* copy-accumulators: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_COPY_ACCUMULATORS,
    /* copy-namespaces: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES,
    /* count: PATTERN. */
    SXML_XSLT_AST_ATTRIBUTE_COUNT,
    /* csv-header: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_CSV_HEADER,
    /* csv-quote-character: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_CSV_QUOTE_CHARACTER,
    /* csv-separator: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_CSV_SEPARATOR,
    /* data-type: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE,
    /* decimal-separator: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_DECIMAL_SEPARATOR,
    /* declared-modes: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_DECLARED_MODES,
    /* default: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_DEFAULT,
    /* digit: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_DIGIT,
    /* disable-output-escaping: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_DISABLE_OUTPUT_ESCAPING,
    /* doctype-public: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_PUBLIC,
    /* doctype-system: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_SYSTEM,
    /* duplicates: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_DUPLICATES,
    /* elements: NAME_LIST. */
    SXML_XSLT_AST_ATTRIBUTE_ELEMENTS,
    /* else: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_ELSE,
    /* encoding: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_ENCODING,
    /* errors: NAME_LIST. */
    SXML_XSLT_AST_ATTRIBUTE_ERRORS,
    /* error-code: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_ERROR_CODE,
    /* escape-solidus: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_ESCAPE_SOLIDUS,
    /* escape-uri-attributes: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_ESCAPE_URI_ATTRIBUTES,
    /* exponent-separator: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_EXPONENT_SEPARATOR,
    /* flags: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_FLAGS,
    /* format: VALUE_TEMPLATE; TEXT on package-location. */
    SXML_XSLT_AST_ATTRIBUTE_FORMAT,
    /* for-each: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_FOR_EACH,
    /* for-each-item: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM,
    /* for-each-source: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE,
    /* from: PATTERN. */
    SXML_XSLT_AST_ATTRIBUTE_FROM,
    /* grouping-separator: TEXT; VALUE_TEMPLATE on number. */
    SXML_XSLT_AST_ATTRIBUTE_GROUPING_SEPARATOR,
    /* grouping-size: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_GROUPING_SIZE,
    /* group-adjacent: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT,
    /* group-by: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_GROUP_BY,
    /* group-ending-with: PATTERN. */
    SXML_XSLT_AST_ATTRIBUTE_GROUP_ENDING_WITH,
    /* group-starting-with: PATTERN. */
    SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH,
    /* href: TEXT; VALUE_TEMPLATE on result-document, source-document. */
    SXML_XSLT_AST_ATTRIBUTE_HREF,
    /* html-version: NUMBER; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_HTML_VERSION,
    /* id: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_ID,
    /* include-content-type: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_INCLUDE_CONTENT_TYPE,
    /* indent: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_INDENT,
    /* indent-attributes: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_INDENT_ATTRIBUTES,
    /* indent-unit: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_INDENT_UNIT,
    /* infinity: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_INFINITY,
    /* inherit-namespaces: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES,
    /* initial-value: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_INITIAL_VALUE,
    /* input-type-annotations: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_INPUT_TYPE_ANNOTATIONS,
    /* is-priority: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_IS_PRIORITY,
    /* item-separator: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_ITEM_SEPARATOR,
    /* json-lines: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_JSON_LINES,
    /* json-node-output-method: KEYWORD, or NAME for a prefixed method;
       VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_JSON_NODE_OUTPUT_METHOD,
    /* key: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_KEY,
    /* lang: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_LANG,
    /* letter-value: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_LETTER_VALUE,
    /* level: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_LEVEL,
    /* line-ending: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_LINE_ENDING,
    /* main-module: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_MAIN_MODULE,
    /* match: PATTERN. */
    SXML_XSLT_AST_ATTRIBUTE_MATCH,
    /* media-type: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_MEDIA_TYPE,
    /* merge-when: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_MERGE_WHEN,
    /* method: KEYWORD, or NAME for a prefixed method; VALUE_TEMPLATE on
       result-document. */
    SXML_XSLT_AST_ATTRIBUTE_METHOD,
    /* minus-sign: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_MINUS_SIGN,
    /* mode: NAME_LIST (one token on apply-templates). */
    SXML_XSLT_AST_ATTRIBUTE_MODE,
    /* name: NAME; VALUE_TEMPLATE on attribute, element, namespace,
       processing-instruction; TEXT on package, use-package. */
    SXML_XSLT_AST_ATTRIBUTE_NAME,
    /* names: NAME_LIST. */
    SXML_XSLT_AST_ATTRIBUTE_NAMES,
    /* namespace: VALUE_TEMPLATE; TEXT on import-schema. */
    SXML_XSLT_AST_ATTRIBUTE_NAMESPACE,
    /* namespace-context: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_NAMESPACE_CONTEXT,
    /* NaN: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_NAN,
    /* new-each-time: KEYWORD (YES, NO, MAYBE). */
    SXML_XSLT_AST_ATTRIBUTE_NEW_EACH_TIME,
    /* normalization-form: KEYWORD, or TEXT for another form;
       VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_NORMALIZATION_FORM,
    /* omit-xml-declaration: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_OMIT_XML_DECLARATION,
    /* on-multiple-match: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_ON_MULTIPLE_MATCH,
    /* on-no-match: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_ON_NO_MATCH,
    /* order: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_ORDER,
    /* ordinal: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_ORDINAL,
    /* output-version: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_OUTPUT_VERSION,
    /* override: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_OVERRIDE,
    /* override-extension-function: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_OVERRIDE_EXTENSION_FUNCTION,
    /* package-version: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION,
    /* parameter-document: TEXT; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_PARAMETER_DOCUMENT,
    /* path-in-archive: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_PATH_IN_ARCHIVE,
    /* pattern-separator: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_PATTERN_SEPARATOR,
    /* percent: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_PERCENT,
    /* per-mille: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_PER_MILLE,
    /* phase: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_PHASE,
    /* priority: NUMBER. */
    SXML_XSLT_AST_ATTRIBUTE_PRIORITY,
    /* regex: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_REGEX,
    /* required: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_REQUIRED,
    /* result-prefix: NAMESPACE_BINDING. */
    SXML_XSLT_AST_ATTRIBUTE_RESULT_PREFIX,
    /* role: NAME. */
    SXML_XSLT_AST_ATTRIBUTE_ROLE,
    /* rollback-output: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_ROLLBACK_OUTPUT,
    /* schema-aware: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_SCHEMA_AWARE,
    /* schema-location: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_SCHEMA_LOCATION,
    /* select: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_SELECT,
    /* separator: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_SEPARATOR,
    /* sort-before-merge: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_SORT_BEFORE_MERGE,
    /* split-when: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_SPLIT_WHEN,
    /* stable: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_STABLE,
    /* standalone: KEYWORD (YES, NO, OMIT); VALUE_TEMPLATE on
       result-document. */
    SXML_XSLT_AST_ATTRIBUTE_STANDALONE,
    /* start-at: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_START_AT,
    /* static: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_STATIC,
    /* streamability: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_STREAMABILITY,
    /* streamable: BOOLEAN (D97: read, then run unstreamed). */
    SXML_XSLT_AST_ATTRIBUTE_STREAMABLE,
    /* string: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_STRING,
    /* stylesheet-prefix: NAMESPACE_BINDING. */
    SXML_XSLT_AST_ATTRIBUTE_STYLESHEET_PREFIX,
    /* suppress-indentation: NAME_LIST; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_SUPPRESS_INDENTATION,
    /* terminate: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_TERMINATE,
    /* test: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_TEST,
    /* then: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_THEN,
    /* trusted: VALUE_TEMPLATE. */
    SXML_XSLT_AST_ATTRIBUTE_TRUSTED,
    /* tunnel: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
    /* type: NAME (XTSE1660 for a type other than xs:untyped and
       xs:untypedAtomic, D97). */
    SXML_XSLT_AST_ATTRIBUTE_TYPE,
    /* typed: KEYWORD (YES, NO, STRICT, LAX, UNSPECIFIED). */
    SXML_XSLT_AST_ATTRIBUTE_TYPED,
    /* undeclare-prefixes: BOOLEAN; VALUE_TEMPLATE on result-document. */
    SXML_XSLT_AST_ATTRIBUTE_UNDECLARE_PREFIXES,
    /* use: KEYWORD; EXPRESSION on key. */
    SXML_XSLT_AST_ATTRIBUTE_USE,
    /* use-accumulators: NAME_LIST. */
    SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS,
    /* use-attribute-sets: NAME_LIST. */
    SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS,
    /* use-character-maps: NAME_LIST. */
    SXML_XSLT_AST_ATTRIBUTE_USE_CHARACTER_MAPS,
    /* validation: KEYWORD (STRICT and LAX are XTSE1660, D97). */
    SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
    /* value: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_VALUE,
    /* visibility: KEYWORD. */
    SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
    /* warning-on-multiple-match: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_MULTIPLE_MATCH,
    /* warning-on-no-match: BOOLEAN. */
    SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_NO_MATCH,
    /* with-params: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_WITH_PARAMS,
    /* xpath: EXPRESSION. */
    SXML_XSLT_AST_ATTRIBUTE_XPATH,
    /* zero-digit: TEXT. */
    SXML_XSLT_AST_ATTRIBUTE_ZERO_DIGIT,

    /* One more than the largest attribute kind (not ATTRIBUTE_COUNT,
       which is the attribute count). */
    SXML_XSLT_AST_ATTRIBUTE_KIND_COUNT,
} sxml_xslt_ast_attribute_kind;

/*
 * The words a KEYWORD attribute takes, from the enumerations of the XSLT
 * 4.0 schema; one value per spelling, shared by every attribute that uses
 * it (FAIL is on-no-match="fail" and on-multiple-match="fail"). The yes/no
 * forms of a three-valued attribute (typed, new-each-time, standalone)
 * are YES and NO for each of their spellings (yes, true, 1; no, false, 0).
 * Values are only ever appended.
 */
typedef enum sxml_xslt_ast_keyword {
    SXML_XSLT_AST_KEYWORD_NONE = 0,
    /* on-no-match (6.10). */
    SXML_XSLT_AST_KEYWORD_DEEP_COPY,
    SXML_XSLT_AST_KEYWORD_SHALLOW_COPY,
    SXML_XSLT_AST_KEYWORD_SHALLOW_COPY_ALL,
    SXML_XSLT_AST_KEYWORD_DEEP_SKIP,
    SXML_XSLT_AST_KEYWORD_SHALLOW_SKIP,
    SXML_XSLT_AST_KEYWORD_TEXT_ONLY_COPY,
    SXML_XSLT_AST_KEYWORD_FAIL,
    /* on-multiple-match. */
    SXML_XSLT_AST_KEYWORD_USE_LAST,
    /* visibility (3.4.5). */
    SXML_XSLT_AST_KEYWORD_PUBLIC,
    SXML_XSLT_AST_KEYWORD_PRIVATE,
    SXML_XSLT_AST_KEYWORD_FINAL,
    SXML_XSLT_AST_KEYWORD_ABSTRACT,
    SXML_XSLT_AST_KEYWORD_HIDDEN,
    /* validation, typed, input-type-annotations. */
    SXML_XSLT_AST_KEYWORD_STRICT,
    SXML_XSLT_AST_KEYWORD_LAX,
    SXML_XSLT_AST_KEYWORD_PRESERVE,
    SXML_XSLT_AST_KEYWORD_STRIP,
    SXML_XSLT_AST_KEYWORD_UNSPECIFIED,
    /* typed, new-each-time, standalone. */
    SXML_XSLT_AST_KEYWORD_YES,
    SXML_XSLT_AST_KEYWORD_NO,
    SXML_XSLT_AST_KEYWORD_MAYBE,
    SXML_XSLT_AST_KEYWORD_OMIT,
    /* method and json-node-output-method (Serialization 4.0). */
    SXML_XSLT_AST_KEYWORD_XML,
    SXML_XSLT_AST_KEYWORD_XHTML,
    SXML_XSLT_AST_KEYWORD_HTML,
    SXML_XSLT_AST_KEYWORD_TEXT,
    SXML_XSLT_AST_KEYWORD_JSON,
    SXML_XSLT_AST_KEYWORD_CSV,
    SXML_XSLT_AST_KEYWORD_ADAPTIVE,
    /* level of xsl:number (12.3). */
    SXML_XSLT_AST_KEYWORD_SINGLE,
    SXML_XSLT_AST_KEYWORD_MULTIPLE,
    SXML_XSLT_AST_KEYWORD_ANY,
    /* use of xsl:context-item and xsl:global-context-item. */
    SXML_XSLT_AST_KEYWORD_REQUIRED,
    SXML_XSLT_AST_KEYWORD_OPTIONAL,
    SXML_XSLT_AST_KEYWORD_ABSENT,
    /* phase of xsl:accumulator-rule. */
    SXML_XSLT_AST_KEYWORD_START,
    SXML_XSLT_AST_KEYWORD_END,
    /* normalization-form; "none" is NO_NORMALIZATION. */
    SXML_XSLT_AST_KEYWORD_NFC,
    SXML_XSLT_AST_KEYWORD_NFD,
    SXML_XSLT_AST_KEYWORD_NFKC,
    SXML_XSLT_AST_KEYWORD_NFKD,
    SXML_XSLT_AST_KEYWORD_FULLY_NORMALIZED,
    SXML_XSLT_AST_KEYWORD_NO_NORMALIZATION,
    /* streamability of xsl:function (read, not used: D97). */
    SXML_XSLT_AST_KEYWORD_UNCLASSIFIED,
    SXML_XSLT_AST_KEYWORD_ABSORBING,
    SXML_XSLT_AST_KEYWORD_INSPECTION,
    SXML_XSLT_AST_KEYWORD_FILTER,
    SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT,
    SXML_XSLT_AST_KEYWORD_DEEP_DESCENT,
    SXML_XSLT_AST_KEYWORD_ASCENT,
    /* component of xsl:accept and xsl:expose; "*" is ANY_COMPONENT. */
    SXML_XSLT_AST_KEYWORD_TEMPLATE,
    SXML_XSLT_AST_KEYWORD_FUNCTION,
    SXML_XSLT_AST_KEYWORD_VARIABLE,
    SXML_XSLT_AST_KEYWORD_ATTRIBUTE_SET,
    SXML_XSLT_AST_KEYWORD_MODE,
    SXML_XSLT_AST_KEYWORD_ITEM_TYPE,
    SXML_XSLT_AST_KEYWORD_RECORD_TYPE,
    SXML_XSLT_AST_KEYWORD_ANY_COMPONENT,
} sxml_xslt_ast_keyword;

/* ----------------------------------------------------------- Name lists */

/* One token of a NAME_LIST value. */
typedef enum sxml_xslt_ast_name_token_kind {
    SXML_XSLT_AST_NAME_TOKEN_KIND_NONE = 0,
    /* An EQName, resolved into name. */
    SXML_XSLT_AST_NAME_TOKEN_KIND_NAME,
    /* "*" (strip-space elements, catch errors, accept names). */
    SXML_XSLT_AST_NAME_TOKEN_KIND_ANY_NAME,
    /* "p:*" or "Q{uri}*": namespace_uri. */
    SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD,
    /* "*:n": name holds n in no namespace; compare local names only. */
    SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD,
    /* "#default": in a mode list, the default mode of the element's
       static context (lowering resolves it, 3.6.3). */
    SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT,
    /* "#unnamed": the unnamed mode. */
    SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED,
    /* "#current": the current mode (apply-templates only). */
    SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT,
    /* "#all": every mode, or every accumulator. */
    SXML_XSLT_AST_NAME_TOKEN_KIND_ALL,
} sxml_xslt_ast_name_token_kind;

typedef struct sxml_xslt_ast_name_token {
    sxml_xslt_ast_name_token_kind kind;
    sxml_name_identifier name;
    sxml_namespace_uri_identifier namespace_uri;
    /* A component test of xsl:accept or xsl:expose names "f:g#2": the
       arity after "#". */
    bool has_arity;
    uint32_t arity;
} sxml_xslt_ast_name_token;

typedef struct sxml_xslt_ast_name_token_view {
    const sxml_xslt_ast_name_token *name_tokens;
    size_t name_token_count;
} sxml_xslt_ast_name_token_view;

typedef struct sxml_xslt_ast_namespace_uri_view {
    const sxml_namespace_uri_identifier *namespace_uris;
    size_t namespace_uri_count;
} sxml_xslt_ast_namespace_uri_view;

/* ---------------------------------------------------------------- Values */

/* The data of a value; the member is given by its kind. */
typedef union sxml_xslt_ast_value_payload {
    /* EXPRESSION, PATTERN, VALUE_TEMPLATE, SEQUENCE_TYPE, ITEM_TYPE. */
    sxml_xslt_ast_expression_index expression;
    sxml_xslt_ast_text text;
    bool is_true;
    double number;
    sxml_xslt_ast_keyword keyword;
    sxml_name_identifier name;
    sxml_xslt_ast_name_token_list name_tokens;
    sxml_namespace_binding namespace_binding;
} sxml_xslt_ast_value_payload;

/* An attribute value, or the content of a TEXT_NODE or SELECT node. */
typedef struct sxml_xslt_ast_value {
    sxml_xslt_ast_value_kind kind;
    sxml_xslt_ast_value_payload payload;
} sxml_xslt_ast_value;

/* ----------------------------------------------- Nodes and attributes */

/* Flags of a node (bits of sxml_xslt_ast_node.flags). */
typedef enum sxml_xslt_ast_node_flag {
    /* Made by the reader, not read from an element: the root and template
       of a simplified module. source_node is the outermost element. */
    SXML_XSLT_AST_NODE_FLAG_SYNTHESIZED = 1U << 0U,
    /* A child of a module root, listed in the declaration list. */
    SXML_XSLT_AST_NODE_FLAG_DECLARATION = 1U << 1U,
} sxml_xslt_ast_node_flag;

/* Flags of an attribute (bits of sxml_xslt_ast_attribute.flags). */
typedef enum sxml_xslt_ast_attribute_flag {
    /* Written in the XSLT namespace (xsl:version on a literal result
       element, 3.3). */
    SXML_XSLT_AST_ATTRIBUTE_FLAG_XSLT_NAMESPACE = 1U << 0U,
    /* The value came from a shadow attribute (3.11.4). */
    SXML_XSLT_AST_ATTRIBUTE_FLAG_SHADOW = 1U << 1U,
    /* Made by the reader: the match "." of a simplified module. */
    SXML_XSLT_AST_ATTRIBUTE_FLAG_SYNTHESIZED = 1U << 2U,
} sxml_xslt_ast_attribute_flag;

/*
 * One stylesheet element or text node. Read through
 * sxml_xslt_ast_get_node; written only by the functions below.
 */
typedef struct sxml_xslt_ast_node {
    sxml_xslt_ast_kind kind;
    /* Bits of sxml_xslt_ast_node_flag. */
    uint32_t flags;
    sxml_xslt_ast_module_index module;
    /* The element or text node of the module's tree this node was read
       from. */
    sxml_node_index source_node;
    sxml_xslt_ast_node_index parent;
    sxml_xslt_ast_node_index first_child;
    sxml_xslt_ast_node_index last_child;
    sxml_xslt_ast_node_index next_sibling;
    sxml_xslt_ast_static_context_index static_context;
    sxml_xslt_ast_attribute_list attributes;
    /* LITERAL_RESULT_ELEMENT, EXTENSION_INSTRUCTION, UNKNOWN_INSTRUCTION,
       DATA_ELEMENT: the element's name and the prefix it was written
       with. 0 for the XSLT kinds, whose kind is their name. */
    sxml_name_identifier element_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    /* TEXT_NODE: TEXT or VALUE_TEMPLATE. SELECT: EXPRESSION. NONE for
       every other kind. */
    sxml_xslt_ast_value content;
    /* INCLUDE and IMPORT: the module read for them (0 for an ignored
       xsl:include). */
    sxml_xslt_ast_module_index included_module;
    /* For a declaration, its position in the declaration list, from 1;
       0 for every other node. */
    uint32_t declaration_order;
    /* Where the start tag (or text) begins in its module: line, column,
       byte offset; byte_count 0. From the tree's locations (line 0 when
       the tree has none). */
    sxml_ast_source_span source_span;
} sxml_xslt_ast_node;

/* One attribute of a node, as read. */
typedef struct sxml_xslt_ast_attribute {
    sxml_xslt_ast_attribute_kind kind;
    /* Bits of sxml_xslt_ast_attribute_flag. */
    uint32_t flags;
    /* The node it belongs to. */
    sxml_xslt_ast_node_index node;
    /* Its expanded name and prefix as in the tree (for a shadow
       attribute, the name without "_"). */
    sxml_name_identifier attribute_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_xslt_ast_value value;
    /* Where the attribute begins in its module; byte_count 0. */
    sxml_ast_source_span source_span;
} sxml_xslt_ast_attribute;

/*
 * One expression, pattern, value template or type: its own sxml_ast
 * (owned by the stylesheet AST), the static context it is compiled in and
 * the node it belongs to. The sxml_ast's source spans count from the
 * first byte of the attribute value or text; its source name is the
 * module URI.
 */
typedef struct sxml_xslt_ast_expression {
    /* EXPRESSION, PATTERN, VALUE_TEMPLATE, SEQUENCE_TYPE or ITEM_TYPE. */
    sxml_xslt_ast_value_kind kind;
    sxml_ast *ast;
    sxml_xslt_ast_static_context_index static_context;
    sxml_xslt_ast_node_index node;
    /* Where the attribute or text holding the text begins in the module;
       byte_count 0. */
    sxml_ast_source_span source_span;
} sxml_xslt_ast_expression;

/* ------------------------------------------------------- Static contexts */

/*
 * What the standard attributes and namespace declarations of a node and
 * its ancestors in the module decide (see above).
 */
typedef struct sxml_xslt_ast_static_context {
    /* The static context it was derived from; 0 at a module root. */
    sxml_xslt_ast_static_context_index parent;
    /* The statically known namespaces of expressions, patterns, types and
       QName-valued attributes, without the predeclared ones and without a
       default namespace binding (prefix 0 never occurs): the fixed
       namespace bindings when the module root has fixed-namespaces
       (3.6.1), else the element's in-scope namespaces. */
    sxml_xslt_ast_namespace_binding_list namespace_bindings;
    /* [xsl:]xpath-default-namespace (5.1.2.1): 0 for none;
       is_any_namespace for "##any" (unprefixed element names match any
       namespace). */
    sxml_namespace_uri_identifier xpath_default_namespace;
    bool is_any_namespace;
    /* The first collation of [xsl:]default-collation that sxml knows
       (3.6.2, XTSE0125 when none); empty for the codepoint collation. */
    sxml_xslt_ast_text default_collation;
    /* [xsl:]default-mode (3.6.3): kind UNNAMED (the default) or NAME. */
    sxml_xslt_ast_name_token default_mode;
    /* [xsl:]default-validation: STRIP (the default) or PRESERVE. */
    sxml_xslt_ast_keyword default_validation;
    /* [xsl:]expand-text (5.6.2). */
    bool expands_text;
    /* [xsl:]version (3.8, 3.9): versions above 4.0 read in
       forwards-compatible mode. */
    double version;
    /* Namespaces whose namespace nodes literal result elements do not copy
       (11.1.3): the XSLT namespace, the extension namespaces and those of
       [xsl:]exclude-result-prefixes, cumulative. */
    sxml_xslt_ast_namespace_uri_list excluded_namespaces;
    /* [xsl:]extension-element-prefixes (24.3), cumulative. */
    sxml_xslt_ast_namespace_uri_list extension_namespaces;
    /* The static base URI (xml:base against the module URI); absolute, or
       empty when the module has none. */
    sxml_xslt_ast_text static_base_uri;
} sxml_xslt_ast_static_context;

/* ----------------------------------------------------------------- Modules */

typedef enum sxml_xslt_ast_module_relation {
    SXML_XSLT_AST_MODULE_RELATION_NONE = 0,
    /* Module 1: the stylesheet the processor was asked to compile. */
    SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL,
    SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
    SXML_XSLT_AST_MODULE_RELATION_IMPORTED,
} sxml_xslt_ast_module_relation;

typedef struct sxml_xslt_ast_module {
    sxml_xslt_ast_module_relation relation;
    /* The module whose xsl:include or xsl:import read this one; 0 for the
       principal module. */
    sxml_xslt_ast_module_index parent_module;
    /* That xsl:include or xsl:import; 0 for the principal module. */
    sxml_xslt_ast_node_index referencing_node;
    /* STYLESHEET or PACKAGE; synthesized for a simplified module. */
    sxml_xslt_ast_node_index root;
    bool is_simplified;
    /* The module's document node: a reference owned by the AST. */
    sxml_item document;
    /* Its tree, borrowed from document; outermost_element is the
       document element. */
    sxml_tree *tree;
    sxml_node_index outermost_element;
    /* The absolute URI the module was read from, or the name the caller
       gave the principal module; owned, in an allocation of its own that
       never moves (the source name of the module's expression ASTs). */
    sxml_byte_view module_uri;
    /* 0 until sxml_xslt_ast_seal; then from 1, higher wins (see above). */
    uint32_t import_precedence;
    /* Modules between this one and the principal module (0 for it), and
       the modules of its subtree in the module tree, itself included;
       kept for sxml_xslt_ast_seal. */
    uint32_t nesting_depth;
    uint32_t subtree_module_count;
} sxml_xslt_ast_module;

/* -------------------------------------------------------------------- AST */

typedef struct sxml_xslt_ast_configuration {
    /* Memory for the arrays as they grow, module URIs and expression ASTs
       added by the reader. Must not be the null allocator; must outlive
       the AST. */
    sxml_allocator allocator;
    /* Module documents are items of this heap and names come from its
       pool. Kept by reference; must outlive the AST. */
    sxml_item_heap *item_heap;
    /* Most modules, a bound for untrusted input: imports of imports can
       read one document exponentially often. 0 means 1,000. Reaching it
       gives SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_module_count;
    /* Most nodes plus attributes; 0 means no bound. Reaching it gives
       SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_node_count;
} sxml_xslt_ast_configuration;

/*
 * A stylesheet AST. The layout is visible for the inline accessors below;
 * only the functions of this header write it. Each array is allocated on
 * the first add, with entry 0 reserved (all zero), so a count is 0 or at
 * least 2.
 */
typedef struct sxml_xslt_ast {
    bool is_sealed;
    /* True when made by sxml_xslt_ast_create. */
    bool is_created;
    sxml_allocator allocator;
    sxml_item_heap *item_heap;
    uint32_t maximum_module_count;
    uint32_t maximum_node_count;

    sxml_xslt_ast_module *modules;
    uint32_t module_count;
    uint32_t module_capacity;
    sxml_xslt_ast_node *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
    sxml_xslt_ast_attribute *attributes;
    uint32_t attribute_count;
    uint32_t attribute_capacity;
    /* Owned ASTs, destroyed when finalized. */
    sxml_xslt_ast_expression *expressions;
    uint32_t expression_count;
    uint32_t expression_capacity;
    sxml_xslt_ast_static_context *static_contexts;
    uint32_t static_context_count;
    uint32_t static_context_capacity;
    /* The declaration list: declarations[n] is the node with
       declaration_order n. */
    sxml_xslt_ast_node_index *declarations;
    uint32_t declaration_count;
    uint32_t declaration_capacity;
    sxml_xslt_ast_name_token *name_tokens;
    uint32_t name_token_count;
    uint32_t name_token_capacity;
    sxml_namespace_binding *namespace_bindings;
    uint32_t namespace_binding_count;
    uint32_t namespace_binding_capacity;
    sxml_namespace_uri_identifier *namespace_uris;
    uint32_t namespace_uri_count;
    uint32_t namespace_uri_capacity;
    uint8_t *text_bytes;
    uint32_t text_byte_count;
    uint32_t text_byte_capacity;

    /* All zero: what the accessors return for index 0 or out of range. */
    sxml_xslt_ast_module no_module;
    sxml_xslt_ast_node no_node;
    sxml_xslt_ast_attribute no_attribute;
    sxml_xslt_ast_expression no_expression;
    sxml_xslt_ast_static_context no_static_context;
} sxml_xslt_ast;

/*
 * The functions below are defined in src/xslt_ast.c, written with the
 * stylesheet reader (packet 4.5), the AST's only producer. They follow the
 * conventions of sxml/memory.h; on failure outputs and the AST are
 * unchanged (D20). Every add function returns SXML_STATUS_INVALID_ARGUMENT
 * on a sealed AST or for an index not in use, SXML_STATUS_LIMIT_EXCEEDED
 * past a configured maximum or UINT32_MAX - 1 entries, and the
 * allocator's failures.
 */

/* Writes the requirements of the AST header (the arrays are allocated as
   it grows) to *requirements. */
SXML_NODISCARD sxml_status sxml_xslt_ast_memory_requirements(
    const sxml_xslt_ast_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places an empty AST (no modules) in memory, which must stay valid,
   unmoved, until sxml_xslt_ast_finalize. */
SXML_NODISCARD sxml_status sxml_xslt_ast_initialize(
    sxml_memory_view memory, const sxml_xslt_ast_configuration *configuration,
    sxml_xslt_ast **ast);

/* Destroys the expression ASTs, releases the module documents and frees
   the arrays and module URIs; accepts NULL. */
void sxml_xslt_ast_finalize(sxml_xslt_ast *ast);

/* Allocates through allocator and initializes; allocator is stored and
   must outlive the AST. */
SXML_NODISCARD sxml_status sxml_xslt_ast_create(
    sxml_allocator allocator, const sxml_xslt_ast_configuration *configuration,
    sxml_xslt_ast **ast);

/* Finalizes an AST made by sxml_xslt_ast_create and frees it; accepts
   NULL. */
void sxml_xslt_ast_destroy(sxml_xslt_ast *ast);

/*
 * Ends building: computes each module's import precedence (see above) and
 * stops all growth, so pointers and views into the AST stay valid for its
 * lifetime. Lowering reads only sealed ASTs.
 */
void sxml_xslt_ast_seal(sxml_xslt_ast *ast);

/* A module to add. */
typedef struct sxml_xslt_ast_module_request {
    /* PRINCIPAL for the first module and only for it. */
    sxml_xslt_ast_module_relation relation;
    /* The module last added or one of its ancestors (pre-order); 0 for
       the principal module. */
    sxml_xslt_ast_module_index parent_module;
    /* The INCLUDE or IMPORT node of parent_module; its included_module is
       set to the new module. */
    sxml_xslt_ast_node_index referencing_node;
    /* A document node; the AST retains it. */
    sxml_item document;
    /* Copied into the module's own allocation. */
    sxml_byte_view module_uri;
} sxml_xslt_ast_module_request;

/* Adds a module (with root 0 until its root node is added) and writes its
   index to *module. */
SXML_NODISCARD sxml_status sxml_xslt_ast_add_module(
    sxml_xslt_ast *ast, const sxml_xslt_ast_module_request *request,
    sxml_xslt_ast_module_index *module);

/* A node to add; it has no parent, children or attributes yet. */
typedef struct sxml_xslt_ast_node_request {
    sxml_xslt_ast_kind kind;
    uint32_t flags;
    sxml_xslt_ast_module_index module;
    sxml_node_index source_node;
    sxml_xslt_ast_static_context_index static_context;
    sxml_name_identifier element_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_xslt_ast_value content;
    sxml_ast_source_span source_span;
} sxml_xslt_ast_node_request;

/*
 * Adds a node and writes its index to *node. The first node of a module
 * becomes its root. A node with flag DECLARATION is appended to the
 * declaration list. Returns SXML_STATUS_INVALID_ARGUMENT for kind NONE.
 */
SXML_NODISCARD sxml_status sxml_xslt_ast_add_node(
    sxml_xslt_ast *ast, const sxml_xslt_ast_node_request *request,
    sxml_xslt_ast_node_index *node);

/* Makes child the last child of parent. */
typedef struct sxml_xslt_ast_child_request {
    sxml_xslt_ast_node_index parent;
    sxml_xslt_ast_node_index child;
} sxml_xslt_ast_child_request;

/* Appends request.child to the children of request.parent; O(1).
   SXML_STATUS_INVALID_ARGUMENT when the child already has a parent or is
   the parent. */
SXML_NODISCARD sxml_status sxml_xslt_ast_append_child(
    sxml_xslt_ast *ast, sxml_xslt_ast_child_request request);

/* An attribute to add to the node added last. */
typedef struct sxml_xslt_ast_attribute_request {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
    uint32_t flags;
    sxml_name_identifier attribute_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_xslt_ast_value value;
    sxml_ast_source_span source_span;
} sxml_xslt_ast_attribute_request;

/*
 * Adds an attribute to request->node, which must be the node added last,
 * so a node's attributes are consecutive. Returns
 * SXML_STATUS_INVALID_ARGUMENT otherwise.
 */
SXML_NODISCARD sxml_status sxml_xslt_ast_add_attribute(
    sxml_xslt_ast *ast, const sxml_xslt_ast_attribute_request *request,
    sxml_xslt_ast_attribute_index *attribute);

/* An expression AST to hand over. */
typedef struct sxml_xslt_ast_expression_request {
    /* EXPRESSION, PATTERN, VALUE_TEMPLATE, SEQUENCE_TYPE or ITEM_TYPE. */
    sxml_xslt_ast_value_kind kind;
    /* Made by a front end with the module URI as source name. */
    sxml_ast *expression_ast;
    sxml_xslt_ast_static_context_index static_context;
    sxml_xslt_ast_node_index node;
    sxml_ast_source_span source_span;
} sxml_xslt_ast_expression_request;

/*
 * Takes ownership of request->expression_ast and writes the expression's
 * index to *expression. On failure the caller keeps ownership.
 */
SXML_NODISCARD sxml_status sxml_xslt_ast_take_expression(
    sxml_xslt_ast *ast, const sxml_xslt_ast_expression_request *request,
    sxml_xslt_ast_expression_index *expression);

/* A static context to add; views are copied. */
typedef struct sxml_xslt_ast_static_context_request {
    sxml_xslt_ast_static_context_index parent;
    sxml_namespace_binding_view namespace_bindings;
    sxml_namespace_uri_identifier xpath_default_namespace;
    bool is_any_namespace;
    sxml_byte_view default_collation;
    sxml_xslt_ast_name_token default_mode;
    sxml_xslt_ast_keyword default_validation;
    bool expands_text;
    double version;
    sxml_xslt_ast_namespace_uri_view excluded_namespaces;
    sxml_xslt_ast_namespace_uri_view extension_namespaces;
    sxml_byte_view static_base_uri;
} sxml_xslt_ast_static_context_request;

SXML_NODISCARD sxml_status sxml_xslt_ast_add_static_context(
    sxml_xslt_ast *ast, const sxml_xslt_ast_static_context_request *request,
    sxml_xslt_ast_static_context_index *static_context);

/* Copies name tokens into the AST, for a NAME_LIST value. */
SXML_NODISCARD sxml_status sxml_xslt_ast_add_name_tokens(
    sxml_xslt_ast *ast, sxml_xslt_ast_name_token_view name_tokens,
    sxml_xslt_ast_name_token_list *list);

/*
 * Copies bytes to the end of the text store. *text is in/out as in
 * sxml_ast_append_text: empty on entry starts a new text, otherwise it
 * must end the store and is extended.
 */
SXML_NODISCARD sxml_status sxml_xslt_ast_append_text(sxml_xslt_ast *ast,
                                                     sxml_byte_view bytes,
                                                     sxml_xslt_ast_text *text);

/*
 * Writes the text form (see above) into output and the bytes it needs to
 * *byte_count; as sxml_ir_unit_write_text, *byte_count is written on
 * SXML_STATUS_CAPACITY_EXCEEDED too and output then holds a prefix.
 */
SXML_NODISCARD sxml_status sxml_xslt_ast_write_text(const sxml_xslt_ast *ast,
                                                    sxml_memory_view output,
                                                    size_t *byte_count);

/* ------------------------------------------------------------- Accessors */

/* The node at index, borrowed; a zero node (kind NONE) for 0 or out of
   range. */
static inline const sxml_xslt_ast_node *sxml_xslt_ast_get_node(
    const sxml_xslt_ast *ast, sxml_xslt_ast_node_index index) {
    if (index.value == 0 || index.value >= ast->node_count) {
        return &ast->no_node;
    }
    return &ast->nodes[index.value];
}

/* The attribute at index, borrowed; a zero attribute for 0 or out of
   range. */
static inline const sxml_xslt_ast_attribute *sxml_xslt_ast_get_attribute(
    const sxml_xslt_ast *ast, sxml_xslt_ast_attribute_index index) {
    if (index.value == 0 || index.value >= ast->attribute_count) {
        return &ast->no_attribute;
    }
    return &ast->attributes[index.value];
}

/* An attribute to look for: the one of kind on node. */
typedef struct sxml_xslt_ast_attribute_query {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
} sxml_xslt_ast_attribute_query;

/*
 * The attribute of query.kind on query.node; index 0 when it has none (an
 * absent optional attribute). Not for LITERAL_RESULT and FOREIGN, which
 * may repeat: walk the node's attribute list for those.
 */
static inline sxml_xslt_ast_attribute_index sxml_xslt_ast_find_attribute(
    const sxml_xslt_ast *ast, sxml_xslt_ast_attribute_query query) {
    const sxml_xslt_ast_attribute_list list =
        sxml_xslt_ast_get_node(ast, query.node)->attributes;
    for (uint32_t offset = 0; offset < list.attribute_count; offset++) {
        const sxml_xslt_ast_attribute_index attribute = {
            .value = list.first_attribute_index + offset};
        if (sxml_xslt_ast_get_attribute(ast, attribute)->kind == query.kind) {
            return attribute;
        }
    }
    return (sxml_xslt_ast_attribute_index){.value = 0};
}

/* The expression at index, borrowed; a zero expression (no AST) for 0 or
   out of range. */
static inline const sxml_xslt_ast_expression *sxml_xslt_ast_get_expression(
    const sxml_xslt_ast *ast, sxml_xslt_ast_expression_index index) {
    if (index.value == 0 || index.value >= ast->expression_count) {
        return &ast->no_expression;
    }
    return &ast->expressions[index.value];
}

/* The static context at index, borrowed; a zero one for 0 or out of
   range. */
static inline const sxml_xslt_ast_static_context *
sxml_xslt_ast_get_static_context(const sxml_xslt_ast *ast,
                                 sxml_xslt_ast_static_context_index index) {
    if (index.value == 0 || index.value >= ast->static_context_count) {
        return &ast->no_static_context;
    }
    return &ast->static_contexts[index.value];
}

/* The module at index, borrowed; a zero module for 0 or out of range. */
static inline const sxml_xslt_ast_module *sxml_xslt_ast_get_module(
    const sxml_xslt_ast *ast, sxml_xslt_ast_module_index index) {
    if (index.value == 0 || index.value >= ast->module_count) {
        return &ast->no_module;
    }
    return &ast->modules[index.value];
}

/* The node with declaration order order (from 1); 0 past the last. */
static inline sxml_xslt_ast_node_index sxml_xslt_ast_get_declaration(
    const sxml_xslt_ast *ast, uint32_t order) {
    if (order == 0 || order >= ast->declaration_count) {
        return (sxml_xslt_ast_node_index){.value = 0};
    }
    return ast->declarations[order];
}

/* The bytes of text, borrowed; empty when text is outside the store. */
static inline sxml_byte_view sxml_xslt_ast_get_text(const sxml_xslt_ast *ast,
                                                    sxml_xslt_ast_text text) {
    if (text.byte_offset > ast->text_byte_count ||
        text.byte_count > ast->text_byte_count - text.byte_offset) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = ast->text_bytes + text.byte_offset,
                            .byte_count = text.byte_count};
}

/* The tokens of list, borrowed; empty when out of range. */
static inline sxml_xslt_ast_name_token_view sxml_xslt_ast_get_name_tokens(
    const sxml_xslt_ast *ast, sxml_xslt_ast_name_token_list list) {
    if (list.name_token_count == 0 ||
        list.first_name_token_index > ast->name_token_count ||
        list.name_token_count >
            ast->name_token_count - list.first_name_token_index) {
        return (sxml_xslt_ast_name_token_view){.name_tokens = NULL,
                                               .name_token_count = 0};
    }
    return (sxml_xslt_ast_name_token_view){
        .name_tokens = ast->name_tokens + list.first_name_token_index,
        .name_token_count = list.name_token_count};
}

/*
 * The statically known namespaces of a static context, borrowed, as the
 * XPath static context takes them (sxml_xpath_static_context
 * .namespace_bindings); empty when out of range.
 */
static inline sxml_namespace_binding_view sxml_xslt_ast_get_namespace_bindings(
    const sxml_xslt_ast *ast, sxml_xslt_ast_static_context_index index) {
    const sxml_xslt_ast_namespace_binding_list list =
        sxml_xslt_ast_get_static_context(ast, index)->namespace_bindings;
    if (list.namespace_binding_count == 0 ||
        list.first_namespace_binding_index > ast->namespace_binding_count ||
        list.namespace_binding_count >
            ast->namespace_binding_count - list.first_namespace_binding_index) {
        return (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    }
    return (sxml_namespace_binding_view){
        .namespace_bindings =
            ast->namespace_bindings + list.first_namespace_binding_index,
        .namespace_binding_count = list.namespace_binding_count};
}

/* The namespace URIs of list (excluded or extension namespaces),
   borrowed; empty when out of range. */
static inline sxml_xslt_ast_namespace_uri_view sxml_xslt_ast_get_namespace_uris(
    const sxml_xslt_ast *ast, sxml_xslt_ast_namespace_uri_list list) {
    if (list.namespace_uri_count == 0 ||
        list.first_namespace_uri_index > ast->namespace_uri_count ||
        list.namespace_uri_count >
            ast->namespace_uri_count - list.first_namespace_uri_index) {
        return (sxml_xslt_ast_namespace_uri_view){.namespace_uris = NULL,
                                                  .namespace_uri_count = 0};
    }
    return (sxml_xslt_ast_namespace_uri_view){
        .namespace_uris = ast->namespace_uris + list.first_namespace_uri_index,
        .namespace_uri_count = list.namespace_uri_count};
}

/*
 * The full source location of node, for an error record (XTSE codes are
 * reported at the element, XPath errors at the expression's node plus the
 * span inside its sxml_ast): the module URI with the node's line, column
 * and byte offset. Borrows the module URI; all zero for index 0.
 */
static inline sxml_source_location sxml_xslt_ast_get_source_location(
    const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_node *node_entry = sxml_xslt_ast_get_node(ast, node);
    const sxml_ast_source_span span = node_entry->source_span;
    return (sxml_source_location){
        .source_name = sxml_xslt_ast_get_module(ast, node_entry->module)->module_uri,
        .line_number = span.line_number,
        .column_number = span.column_number,
        .byte_offset = span.byte_offset};
}
