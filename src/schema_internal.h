#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * The schema components of XSD 1.1 and the stages that compile them
 * (docs/DECISIONS.md D165, D167). Internal to sxml.
 *
 * Section numbers are of XSD 1.1 Part 1 (W3C Recommendation, 5 April
 * 2012) unless they name Part 2.
 *
 * The component model (2.2)
 * -------------------------
 * A schema is a set of components (2.2, 3.17.1): type definitions (simple
 * and complex, one table, as both share one symbol space), element and
 * attribute declarations, attribute uses, attribute groups, model group
 * definitions, model groups, particles, schema wildcards,
 * identity-constraint definitions, type alternatives, assertions,
 * notation declarations and constraining facets. Each kind is a table of
 * sxml_schema_components (glossary schema_components), addressed by an
 * index type of its own; entry 0 of every table is reserved and all zero,
 * so index 0 means "none" and an accessor gives the zero entry for it.
 * Annotations are not kept (D165: sxml has no PSVI annotation property).
 * XSD's property records are not components: the content type, the scope,
 * the value constraint, the open content and the type table are fields of
 * the component that has them, and XPath expression records and the
 * namespace contexts of QName values have tables of their own, as do the
 * schema references, the lists (runs of indexes), the texts and the
 * namespace bindings. The names of components are expanded names of the
 * processor's name pool, so {target namespace} is the namespace of the
 * name.
 *
 * Fields are of two kinds, and every field says which:
 * - "as read": what the XML representation says, written by the read
 *   stage (packet 5.4). A QName attribute (type, base, ref, itemType,
 *   memberTypes, substitutionGroup, refer, defaultAttributes) is a schema
 *   reference (glossary schema_reference): a symbol space, a name and a
 *   position in the reference table. Children known when they are read
 *   (local declarations, anonymous types, model groups, particles, schema
 *   wildcards, facets, attribute uses, XPath records) are linked by index
 *   at once.
 * - "property": the component's property as XSD defines it, written by
 *   the stage named in the comment and read by every stage after it. No
 *   stage after RESOLVE reads a schema reference: references only keep
 *   their names for error messages.
 *
 * Built-in components (3.2.7, 3.4.7, 3.16.7)
 * --------------------------------------------
 * sxml_schema_components_add_builtin_components adds, before anything
 * else, the components every schema has (glossary builtin_component), so
 * their indexes are the same in every schema:
 * - type definitions 1 to 54: the built-in types of the type registry,
 *   with the index of their sxml_builtin_type (type definition 6 is
 *   xs:string, registered type SXML_TYPE_XS_STRING), xs:anyType's base
 *   being itself (3.4.7). xs:untyped (2), xs:untypedAtomic (5) and
 *   xs:numeric (53) are XDM's, not XSD's: they have components but no
 *   entry in the name index, so a schema document cannot name them;
 * - type definition 55: the anonymous list of xs:anyURI that is the type
 *   of xsi:schemaLocation, not built into the registry (is_builtin false),
 *   so the simple types stage registers it as it does a schema's types;
 * - particle 1 (1..1, model group 1), model group 1 (a sequence of
 *   particle 2), particle 2 (0..unbounded, schema wildcard 1) and schema
 *   wildcards 1 (elements: any, lax) and 2 (attributes: any, lax): the
 *   content type and attribute wildcard of xs:anyType and xs:untyped;
 * - attribute declarations 1 to 4: xsi:type (xs:QName), xsi:nil
 *   (xs:boolean), xsi:schemaLocation (type 55) and
 *   xsi:noNamespaceSchemaLocation (xs:anyURI).
 * A schema's own components follow: type definitions from 56, particles
 * from 3, model groups from 2, schema wildcards from 3, attribute
 * declarations from 5, every other table from 1.
 *
 * The compile stages
 * ------------------
 * sxml_schema_initialize (src/schema.c, packet 5.4) makes the components,
 * adds the built-in components and runs the compile stages (glossary
 * compile_stage) in this order, each a function of a file of its own, so
 * that packets add files, not lines, and each stage reads only what the
 * stages before it wrote:
 *
 *     READ                  src/schema_read.c, src/schema_assemble.c (5.4)
 *                           sxml_schema_read_documents
 *     RESOLVE               src/schema_resolve.c (5.4)
 *                           sxml_schema_resolve_references
 *     SIMPLE_TYPES          src/schema_simple_type.c (5.5)
 *                           sxml_schema_compile_simple_types
 *     COMPLEX_TYPES         src/schema_complex_type.c,
 *                           src/schema_content_model.c (5.6)
 *                           sxml_schema_compile_complex_types
 *     IDENTITY_CONSTRAINTS  src/schema_identity.c (5.8)
 *                           sxml_schema_compile_identity_constraints
 *     ASSERTIONS            src/schema_assertion.c (5.9)
 *                           sxml_schema_compile_assertions
 *
 * then seals the components (nothing grows any more, so pointers into
 * them stay valid for the schema's lifetime). A stage reports each error
 * it finds through sxml_schema_compilation_report and goes on while the
 * report sink lets it; compilation fails with SXML_STATUS_STATIC_ERROR
 * after a stage that reported any error, before the next stage runs.
 *
 * - READ parses every schema document with the XML parser into an arena
 *   tree (D12) and walks it with an explicit stack (D71), as
 *   src/xslt_read.c walks a stylesheet: conditional inclusion (4.2.2),
 *   the XML representation constraints (src-*, C.3), the mapping rules of
 *   every xs: element into components "as read", with the schema
 *   document's defaults already applied (elementFormDefault and
 *   attributeFormDefault to local names, blockDefault and finalDefault to
 *   derivation sets, xpathDefaultNamespace to namespace contexts,
 *   defaultOpenContent to the open content of complex types,
 *   defaultAttributes to default_attribute_group_reference). Assembly
 *   (src/schema_assemble.c) follows xs:include, xs:import, xs:redefine
 *   and xs:override through the resource resolver (4.2.3 to 4.2.6),
 *   reads each document once, gives chameleon includes their includer's
 *   target namespace, and enters every global component in the name
 *   index: a redefined or overridden component replaces the original's
 *   entry (sxml_schema_components_replace_name), and a redefinition's
 *   reference to itself is resolved to the original when read. The
 *   document trees are released when READ returns.
 * - RESOLVE turns every schema reference into a component index through
 *   the name index (src-resolve), writes the properties that need only
 *   that (the type definition of declarations without one: the first
 *   substitution group head's, else xs:anyType, 3.3.2.1; attribute
 *   declarations without one: xs:anySimpleType), the substitution group
 *   affiliations and, transitively, the members of every head, and
 *   checks the circularity of model group definitions, attribute groups
 *   and type derivations (mg-props-correct.2, src-attribute_group.3,
 *   ct-props-correct.3, st-props-correct.2).
 * - SIMPLE_TYPES checks every simple type definition (Part 2, 4.1.6 and
 *   the facets' constraints of 4.3), registers it with
 *   sxml_type_registry_register_schema_type in derivation order (D165),
 *   writes primitive_type, the effective assertions and registered_type,
 *   and checks the value constraints of attribute declarations and uses
 *   (a-props-correct, au-props-correct).
 * - COMPLEX_TYPES derives the properties of every complex type
 *   definition from its base and its explicit content (3.4.2): content
 *   type variety, particle, simple content type (synthesizing a simple
 *   type for a restriction with facets, compiled by
 *   sxml_schema_compile_simple_type), open content, effective attribute
 *   uses and attribute wildcard (attribute groups flattened, uses
 *   prohibited by restriction dropped, cos-aw-union and
 *   cos-aw-intersect), effective assertions; checks the derivation
 *   (cos-ct-extends, derivation-ok-restriction), the particles
 *   (p-props-correct, cos-all-limited, cos-element-consistent,
 *   cos-nonambig) and the element declarations (e-props-correct, the
 *   type table and the substitution group, cos-equiv-derived-ok-rec);
 *   compiles each content model (below) and registers the type.
 * - IDENTITY_CONSTRAINTS checks the selector and field XPath subsets
 *   (c-selector-xpath, c-fields-xpath) on the XPath front end's AST and
 *   compiles them, and checks referenced keys (c-props-correct).
 * - ASSERTIONS compiles the tests of assertions and type alternatives
 *   with include/sxml/xpath.h in their namespace context (the XPath of
 *   XSD 1.1 read by the XPath 4.0 front end, a superset).
 * Until a stage's packet fills it, its stub checks nothing and returns
 * SXML_STATUS_OK, so a schema compiles with what the earlier stages
 * check; the validator's calls into a stub return
 * SXML_STATUS_UNSUPPORTED_FEATURE where the feature is used (D162: one
 * rejection point per feature).
 *
 * Content models (plan for packet 5.6)
 * ------------------------------------
 * COMPLEX_TYPES compiles the particle of every complex type with
 * element-only or mixed content into a content model (glossary
 * content_model), opaque outside src/schema_content_model.c and allocated
 * with sxml_schema_components_allocate_block:
 * - a sequence or choice particle tree becomes a deterministic position
 *   automaton (Glushkov): one state per element or wildcard particle
 *   occurrence plus the start state, transitions labelled by an element
 *   declaration or a schema wildcard. A particle whose maxOccurs is small
 *   is expanded into copies; a larger one (or one whose copies would pass
 *   maximum_content_model_state_count) keeps one copy and a content
 *   model counter (glossary content_model_counter): the transition that
 *   repeats it requires the counter below maxOccurs and increments it,
 *   the transitions that leave it require at least minOccurs and reset
 *   it, as src/regex.c runs a large counted repetition as a loop with a
 *   counter (D147). Unique Particle Attribution (cos-nonambig, 3.8.6.4)
 *   is the check that no state has two transitions that can match one
 *   name under one counter condition, an element transition winning over
 *   a wildcard transition (XSD 1.1, 3.8.6.4 note); the matcher then never
 *   backtracks;
 * - an all group (3.8.4.1.3, XSD 1.1: wildcards, maxOccurs above 1) is a
 *   table of its particles with one counter each;
 * - open content (3.4.4.2) is a fallback of the matcher: a name that no
 *   transition takes and the open content wildcard allows stays in the
 *   state (interleave) or, in a final state, moves to the suffix state,
 *   which accepts only the wildcard (suffix);
 * - substitution groups: a transition labelled with a head declaration
 *   matches the name of the head and of each member that may substitute
 *   for it (not abstract, not blocked; cos-equiv-derived-ok-rec), and the
 *   match gives the member's declaration;
 * - restriction of content (derivation-ok-restriction, 3.4.6.4 in XSD
 *   1.1: every sequence the restriction accepts the base accepts) is a
 *   walk of the product of the two automata.
 * The validator (packet 5.7) steps a content model cursor through an
 * element's children: sxml_schema_content_model_start,
 * sxml_schema_content_model_step per child element, and
 * sxml_schema_content_model_can_end at the end; on a mismatch,
 * sxml_schema_content_model_list_expected names what would have been
 * accepted (cvc-complex-type.2.4 messages).
 *
 * The validator's seams (packets 5.7 to 5.9)
 * ------------------------------------------
 * The schema validator (src/schema_validate.c, packet 5.7) is a tree
 * builder (include/sxml/schema.h) whose element stack holds, per open
 * element, its governing declaration and type, a content model cursor,
 * the attributes it inherits (for type alternatives) and the state of
 * its simple content. It calls, never through function pointers (D71):
 * - the content model matcher above (packet 5.6);
 * - sxml_schema_select_type_alternative once an element's attributes are
 *   known (packet 5.9);
 * - sxml_schema_check_assertions when an element whose type has
 *   assertions ends, and for simple values whose type has assertion
 *   facets (packet 5.9);
 * - sxml_schema_check_identity_constraints when an element whose
 *   declaration has identity constraints ends (packet 5.8).
 * Assertions and identity constraints evaluate XPath on a recorded
 * subtree (glossary recorded_subtree): while an element that needs one is
 * open, the validator copies the annotated events it sends into an arena
 * tree as well, one tree for the outermost such element, whose inner
 * ones are nodes of it. The ID table (glossary id_table) collects IDs and
 * IDREFs per validation root and is checked at its end (cvc-id).
 *
 * Text form
 * ---------
 * Golden tests print components (sxml_schema_components_write_text,
 * src/schema_text.c, written with the reader by packet 5.4), one per line in table order and
 * index order, the built-in components left out, each with its position
 * (line:column of the start tag; document:line:column when the schema
 * has more than one document). References to components print their
 * index and, for named ones, the name with the prefix the example's
 * document binds; built-in types print as their QName. explicit-content
 * is printed only where it differs from the content's particle.
 * Properties not yet
 * written by a stage print as for a zero entry, so the text of each stage
 * shows what it adds. The examples below are what the text form shows
 * after the last stage.
 *
 * Example 1: a purchase order (XML Schema Part 0, 2.1, shortened, with a
 * key)
 * --------------------------------------------------------------------
 *
 *   po.xsd:
 *      1  <xsd:schema xmlns:xsd="http://www.w3.org/2001/XMLSchema">
 *      2    <xsd:element name="purchaseOrder" type="PurchaseOrderType">
 *      3      <xsd:key name="partKey">
 *      4        <xsd:selector xpath="items/item"/>
 *      5        <xsd:field xpath="@partNum"/>
 *      6      </xsd:key>
 *      7    </xsd:element>
 *      8    <xsd:element name="comment" type="xsd:string"/>
 *      9    <xsd:complexType name="PurchaseOrderType">
 *     10      <xsd:sequence>
 *     11        <xsd:element name="shipTo" type="USAddress"/>
 *     12        <xsd:element name="billTo" type="USAddress"/>
 *     13        <xsd:element ref="comment" minOccurs="0"/>
 *     14        <xsd:element name="items" type="Items"/>
 *     15      </xsd:sequence>
 *     16      <xsd:attribute name="orderDate" type="xsd:date"/>
 *     17    </xsd:complexType>
 *     18    <xsd:complexType name="USAddress">
 *     19      <xsd:sequence>
 *     20        <xsd:element name="name" type="xsd:string"/>
 *     21        <xsd:element name="street" type="xsd:string"/>
 *     22        <xsd:element name="zip" type="xsd:decimal"/>
 *     23      </xsd:sequence>
 *     24      <xsd:attribute name="country" type="xsd:NMTOKEN" fixed="US"/>
 *     25    </xsd:complexType>
 *     26    <xsd:complexType name="Items">
 *     27      <xsd:sequence>
 *     28        <xsd:element name="item" minOccurs="0" maxOccurs="unbounded">
 *     29          <xsd:complexType>
 *     30            <xsd:sequence>
 *     31              <xsd:element name="productName" type="xsd:string"/>
 *     32              <xsd:element name="quantity">
 *     33                <xsd:simpleType>
 *     34                  <xsd:restriction base="xsd:positiveInteger">
 *     35                    <xsd:maxExclusive value="100"/>
 *     36                  </xsd:restriction>
 *     37                </xsd:simpleType>
 *     38              </xsd:element>
 *     39              <xsd:element ref="comment" minOccurs="0"/>
 *     40            </xsd:sequence>
 *     41            <xsd:attribute name="partNum" type="SKU" use="required"/>
 *     42          </xsd:complexType>
 *     43        </xsd:element>
 *     44      </xsd:sequence>
 *     45    </xsd:complexType>
 *     46    <xsd:simpleType name="SKU">
 *     47      <xsd:restriction base="xsd:string">
 *     48        <xsd:pattern value="\d{3}-[A-Z]{2}"/>
 *     49      </xsd:restriction>
 *     50    </xsd:simpleType>
 *     51  </xsd:schema>
 *
 * The reader adds components in document order of their start tags, a
 * particle before its term (an xs:element or xs:sequence in content is
 * both) and an attribute use before its declaration; references get the
 * next index of the reference table as they are read. No name has a
 * namespace (no targetNamespace) and the forms are unqualified.
 *
 *     document 1 "po.xsd" target-namespace none
 *     context 1 namespaces (xsd = http://www.w3.org/2001/XMLSchema)
 *       default-namespace none base-uri "po.xsd"
 *     type 56 PurchaseOrderType complex base 1 xsd:anyType restriction
 *       content element-only particle 3 attribute-uses (1)
 *       registered at 9:3
 *     type 57 USAddress complex base 1 xsd:anyType restriction
 *       content element-only particle 8 attribute-uses (2)
 *       registered at 18:3
 *     type 58 Items complex base 1 xsd:anyType restriction
 *       content element-only particle 12 registered at 26:3
 *     type 59 anonymous complex base 1 xsd:anyType restriction
 *       content element-only particle 14 attribute-uses (3)
 *       registered at 29:9
 *     type 60 anonymous atomic base 37 xsd:positiveInteger restriction
 *       primitive 8 xsd:decimal facets (1) registered at 33:15
 *     type 61 SKU atomic base 6 xsd:string restriction
 *       primitive 6 xsd:string facets (2) registered at 46:3
 *     element 1 purchaseOrder global type 56 PurchaseOrderType
 *       identity-constraints (1) at 2:3
 *     element 2 comment global type 6 xsd:string at 8:3
 *     element 3 shipTo local in type 56 type 57 USAddress at 11:7
 *     element 4 billTo local in type 56 type 57 USAddress at 12:7
 *     element 5 items local in type 56 type 58 Items at 14:7
 *     element 6 name local in type 57 type 6 xsd:string at 20:7
 *     element 7 street local in type 57 type 6 xsd:string at 21:7
 *     element 8 zip local in type 57 type 8 xsd:decimal at 22:7
 *     element 9 item local in type 58 type 59 at 28:7
 *     element 10 productName local in type 59 type 6 xsd:string at 31:13
 *     element 11 quantity local in type 59 type 60 at 32:13
 *     attribute 5 orderDate local in type 56 type 14 xsd:date at 16:5
 *     attribute 6 country local in type 57 type 41 xsd:NMTOKEN at 24:5
 *     attribute 7 partNum local in type 59 type 61 SKU at 41:11
 *     attribute-use 1 optional attribute 5 at 16:5
 *     attribute-use 2 optional attribute 6 fixed "US" context 1 at 24:5
 *     attribute-use 3 required attribute 7 at 41:11
 *     model-group 2 sequence (particle 4, particle 5, particle 6,
 *       particle 7) at 10:5
 *     model-group 3 sequence (particle 9, particle 10, particle 11)
 *       at 19:5
 *     model-group 4 sequence (particle 13) at 27:5
 *     model-group 5 sequence (particle 15, particle 16, particle 17)
 *       at 30:11
 *     particle 3 1..1 model-group 2 at 10:5
 *     particle 4 1..1 element 3 at 11:7
 *     particle 5 1..1 element 4 at 12:7
 *     particle 6 0..1 element 2 comment (reference 5) at 13:7
 *     particle 7 1..1 element 5 at 14:7
 *     particle 8 1..1 model-group 3 at 19:5
 *     particle 9 1..1 element 6 at 20:7
 *     particle 10 1..1 element 7 at 21:7
 *     particle 11 1..1 element 8 at 22:7
 *     particle 12 1..1 model-group 4 at 27:5
 *     particle 13 0..unbounded element 9 at 28:7
 *     particle 14 1..1 model-group 5 at 30:11
 *     particle 15 1..1 element 10 at 31:13
 *     particle 16 1..1 element 11 at 32:13
 *     particle 17 0..1 element 2 comment (reference 14) at 39:13
 *     identity-constraint 1 partKey key selector xpath 1 fields (xpath 2)
 *       at 3:5
 *     facet 1 max-exclusive "100" context 1 at 35:19
 *     facet 2 pattern "\d{3}-[A-Z]{2}" context 1 at 48:7
 *     xpath 1 context 1 "items/item" compiled at 4:7
 *     xpath 2 context 1 "@partNum" compiled at 5:7
 *     reference 1 type PurchaseOrderType -> 56 at 2:3
 *     reference 2 type xsd:string -> 6 at 8:3
 *     reference 3 type USAddress -> 57 at 11:7
 *     reference 4 type USAddress -> 57 at 12:7
 *     reference 5 element comment -> 2 at 13:7
 *     reference 6 type Items -> 58 at 14:7
 *     reference 7 type xsd:date -> 14 at 16:5
 *     reference 8 type xsd:string -> 6 at 20:7
 *     reference 9 type xsd:string -> 6 at 21:7
 *     reference 10 type xsd:decimal -> 8 at 22:7
 *     reference 11 type xsd:NMTOKEN -> 41 at 24:5
 *     reference 12 type xsd:string -> 6 at 31:13
 *     reference 13 type xsd:positiveInteger -> 37 at 34:17
 *     reference 14 element comment -> 2 at 39:13
 *     reference 15 type SKU -> 61 at 41:11
 *     reference 16 type xsd:string -> 6 at 47:5
 *
 * Reference 1 is the type_reference of element 1, reference 13 the
 * base_type_reference of type 60, reference 5 the term_reference of
 * particle 6, and so on; RESOLVE wrote each arrow and the index field
 * beside the reference. The content models COMPLEX_TYPES compiles, in the
 * plan's text (state 1 is the start; "final" states may end the content):
 *
 *     content-model of type 56 states 5 counters 0
 *       state 1: shipTo element 3 -> 2
 *       state 2: billTo element 4 -> 3
 *       state 3: comment element 2 -> 4, items element 5 -> 5
 *       state 4: items element 5 -> 5
 *       state 5 final
 *     content-model of type 57 states 4 counters 0
 *       state 1: name element 6 -> 2
 *       state 2: street element 7 -> 3
 *       state 3: zip element 8 -> 4
 *       state 4 final
 *     content-model of type 58 states 2 counters 0
 *       state 1 final: item element 9 -> 2
 *       state 2 final: item element 9 -> 2
 *     content-model of type 59 states 4 counters 0
 *       state 1: productName element 10 -> 2
 *       state 2: quantity element 11 -> 3
 *       state 3 final: comment element 2 -> 4
 *       state 4 final
 *
 * Type 60 is registered as an atomic type with base xs:positiveInteger
 * and facet MAX_EXCLUSIVE "100"; type 61 with base xs:string and facet
 * PATTERN "\d{3}-[A-Z]{2}" (canonical facet values, types.h). Every type
 * without a complexContent or simpleContent child is a restriction of
 * xs:anyType (3.4.2.3.3), so its attribute uses are its own.
 *
 * Example 2: a recursive substitution group
 * -----------------------------------------
 *
 *   expression.xsd:
 *      1  <xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
 *      2      xmlns:e="urn:example:expression"
 *      3      targetNamespace="urn:example:expression"
 *      4      elementFormDefault="qualified">
 *      5    <xs:complexType name="Expression" abstract="true"/>
 *      6    <xs:element name="expression" type="e:Expression"
 *      7        abstract="true"/>
 *      8    <xs:complexType name="Operation">
 *      9      <xs:complexContent>
 *     10        <xs:extension base="e:Expression">
 *     11          <xs:sequence>
 *     12            <xs:element ref="e:expression" maxOccurs="unbounded"/>
 *     13          </xs:sequence>
 *     14        </xs:extension>
 *     15      </xs:complexContent>
 *     16    </xs:complexType>
 *     17    <xs:element name="number" substitutionGroup="e:expression">
 *     18      <xs:complexType>
 *     19        <xs:complexContent>
 *     20          <xs:extension base="e:Expression">
 *     21            <xs:attribute name="value" type="xs:decimal"
 *     22                use="required"/>
 *     23          </xs:extension>
 *     24        </xs:complexContent>
 *     25      </xs:complexType>
 *     26    </xs:element>
 *     27    <xs:element name="sum" type="e:Operation"
 *     28        substitutionGroup="e:expression"/>
 *     29    <xs:element name="product" type="e:Operation"
 *     30        substitutionGroup="e:expression"/>
 *     31    <xs:element name="difference" type="e:Operation"
 *     32        substitutionGroup="e:sum"/>
 *     33  </xs:schema>
 *
 * Every name is in urn:example:expression (e:). Operation's content
 * refers to the head, whose members have type Operation: the recursion is
 * in the data, not in compilation, since a particle's term is an index.
 *
 *     document 1 "expression.xsd" target-namespace urn:example:expression
 *     context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema,
 *       e = urn:example:expression) default-namespace none
 *       base-uri "expression.xsd"
 *     type 56 e:Expression complex abstract base 1 xs:anyType restriction
 *       content empty registered at 5:3
 *     type 57 e:Operation complex base 56 e:Expression extension
 *       content element-only particle 3 registered at 8:3
 *     type 58 anonymous complex base 56 e:Expression extension
 *       content empty attribute-uses (1) registered at 18:5
 *     element 1 e:expression global abstract type 56 e:Expression
 *       members (2, 3, 4, 5) at 6:3
 *     element 2 e:number global type 58 substitution-group (1) at 17:3
 *     element 3 e:sum global type 57 e:Operation substitution-group (1)
 *       members (5) at 27:3
 *     element 4 e:product global type 57 e:Operation
 *       substitution-group (1) at 29:3
 *     element 5 e:difference global type 57 e:Operation
 *       substitution-group (3) at 31:3
 *     attribute 5 value local in type 58 type 8 xs:decimal at 21:11
 *     attribute-use 1 required attribute 5 at 21:11
 *     model-group 2 sequence (particle 4) at 11:9
 *     particle 3 1..1 model-group 2 at 11:9
 *     particle 4 1..unbounded element 1 e:expression (reference 3)
 *       at 12:11
 *
 * The base Expression has empty content, so an extension's content is
 * its explicit content (3.4.2.3.3, 4.2.1); type 58 has none and stays
 * empty. RESOLVE writes the affiliations (number, sum and product to
 * expression; difference to sum) and the members, transitively: sum's
 * (difference) are expression's too. Every member's type derives from
 * Expression by extension and nothing blocks extension, so all four may
 * substitute (cos-equiv-derived-ok-rec):
 *
 *     content-model of type 57 states 2 counters 0
 *       state 1: e:expression element 1 -> 2
 *       state 2 final: e:expression element 1 -> 2
 *
 * A child e:difference takes the transition of element 1 and the match
 * gives element 5, whose type (57) then governs it.
 *
 * Example 3: XSD 1.1: an assertion facet, assertions, open content, a
 * counter and type alternatives
 * ------------------------------------------------------------------
 *
 *   message.xsd:
 *      1  <xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
 *      2      xmlns:m="urn:example:message" targetNamespace="urn:example:message"
 *      3      elementFormDefault="qualified"
 *      4      xpathDefaultNamespace="##targetNamespace">
 *      5    <xs:simpleType name="Percent">
 *      6      <xs:restriction base="xs:decimal">
 *      7        <xs:minInclusive value="0"/>
 *      8        <xs:maxInclusive value="100" fixed="true"/>
 *      9        <xs:assertion test="$value mod 5 = 0"/>
 *     10      </xs:restriction>
 *     11    </xs:simpleType>
 *     12    <xs:complexType name="Message">
 *     13      <xs:openContent mode="suffix">
 *     14        <xs:any namespace="##other" processContents="skip"/>
 *     15      </xs:openContent>
 *     16      <xs:sequence>
 *     17        <xs:element name="subject" type="xs:string"/>
 *     18        <xs:element name="line" type="xs:string" maxOccurs="1000"/>
 *     19      </xs:sequence>
 *     20      <xs:attribute name="kind" type="xs:token" use="required"/>
 *     21      <xs:attribute name="discount" type="m:Percent"/>
 *     22      <xs:assert test="@kind ne 'bulk' or count(line) le 10"/>
 *     23    </xs:complexType>
 *     24    <xs:complexType name="Urgent">
 *     25      <xs:complexContent>
 *     26        <xs:extension base="m:Message">
 *     27          <xs:sequence>
 *     28            <xs:element name="deadline" type="xs:dateTime"/>
 *     29          </xs:sequence>
 *     30        </xs:extension>
 *     31      </xs:complexContent>
 *     32    </xs:complexType>
 *     33    <xs:element name="message" type="m:Message">
 *     34      <xs:alternative test="@kind = 'urgent'" type="m:Urgent"/>
 *     35      <xs:alternative type="m:Message"/>
 *     36    </xs:element>
 *     37  </xs:schema>
 *
 *     document 1 "message.xsd" target-namespace urn:example:message
 *     context 1 namespaces (xs = http://www.w3.org/2001/XMLSchema,
 *       m = urn:example:message) default-namespace urn:example:message
 *       base-uri "message.xsd"
 *     type 56 m:Percent atomic base 8 xs:decimal restriction
 *       primitive 8 xs:decimal facets (1, 2) assertions (1) registered
 *       at 5:3
 *     type 57 m:Message complex base 1 xs:anyType restriction
 *       content element-only particle 3
 *       open-content suffix wildcard 3 attribute-uses (1, 2)
 *       assertions (2) registered at 12:3
 *     type 58 m:Urgent complex base 57 m:Message extension
 *       explicit-content particle 6 content element-only particle 8
 *       open-content suffix wildcard 3 attribute-uses (1, 2)
 *       assertions (2) registered at 24:3
 *     element 1 m:subject local in type 57 type 6 xs:string at 17:7
 *     element 2 m:line local in type 57 type 6 xs:string at 18:7
 *     element 3 m:deadline local in type 58 type 12 xs:dateTime at 28:11
 *     element 4 m:message global type 57 m:Message
 *       alternatives (1, 2) at 33:3
 *     attribute 5 kind local in type 57 type 39 xs:token at 20:5
 *     attribute 6 discount local in type 57 type 56 m:Percent at 21:5
 *     attribute-use 1 required attribute 5 at 20:5
 *     attribute-use 2 optional attribute 6 at 21:5
 *     model-group 2 sequence (particle 4, particle 5) at 16:5
 *     model-group 3 sequence (particle 7) at 27:9
 *     model-group 4 sequence (particle 3, particle 6) at 24:3
 *     particle 3 1..1 model-group 2 at 16:5
 *     particle 4 1..1 element 1 at 17:7
 *     particle 5 1..1000 element 2 at 18:7
 *     particle 6 1..1 model-group 3 at 27:9
 *     particle 7 1..1 element 3 at 28:11
 *     particle 8 1..1 model-group 4 at 24:3
 *     wildcard 3 not (urn:example:message, absent) skip at 14:7
 *     alternative 1 test xpath 3 type 58 m:Urgent at 34:5
 *     alternative 2 type 57 m:Message at 35:5
 *     assertion 1 test xpath 1 at 9:7
 *     assertion 2 test xpath 2 at 22:5
 *     facet 1 min-inclusive "0" context 1 at 7:7
 *     facet 2 max-inclusive "100" fixed context 1 at 8:7
 *     xpath 1 context 1 "$value mod 5 = 0" compiled at 9:7
 *     xpath 2 context 1 "@kind ne 'bulk' or count(line) le 10" compiled
 *       at 22:5
 *     xpath 3 context 1 "@kind = 'urgent'" compiled at 34:5
 *
 * ##other is "not the target namespace and not absent" in XSD 1.1
 * (3.10.2.2). Particle 8 and model group 4 are the components
 * COMPLEX_TYPES synthesizes for the extension (3.4.2.3.3: a sequence of
 * the base's particle and the explicit content), so they come after
 * everything READ added; Urgent inherits Message's open content,
 * attribute uses and assertions. xs:assertion is not a registry facet
 * (types.h): type 56 is registered with MIN_INCLUSIVE "0" and
 * MAX_INCLUSIVE "100", and its assertion stays here for validation (and,
 * in schema-aware expressions, for the cast, packet 5.11). Every XPath
 * record has the default namespace of xpathDefaultNamespace, so `line`
 * in xpath 2 is m:line. Alternative 2 has no test: it is the default
 * type definition of element 4's type table (3.12.2). Particle 5's
 * maxOccurs is too large to expand, so its repetitions are counted:
 *
 *     content-model of type 57 states 4 counters 1
 *       counter 1 1..1000
 *       state 1: m:subject element 1 -> 2
 *       state 2: m:line element 2 -> 3 [counter 1 = 1]
 *       state 3 final if counter 1 >= 1:
 *         m:line element 2 -> 3 [counter 1 < 1000, counter 1 + 1]
 *       state 4 final suffix
 *       open-content suffix wildcard 3 -> 4
 *     content-model of type 58 states 5 counters 1
 *       counter 1 1..1000
 *       state 1: m:subject element 1 -> 2
 *       state 2: m:line element 2 -> 3 [counter 1 = 1]
 *       state 3: m:line element 2 -> 3 [counter 1 < 1000, counter 1 + 1],
 *         m:deadline element 3 -> 4 [counter 1 >= 1]
 *       state 4 final
 *       state 5 final suffix
 *       open-content suffix wildcard 3 -> 5
 *
 * Validating <message kind="urgent"> evaluates xpath 3 on the element
 * with its attributes and selects type 58; at the element's end xpath 2
 * runs on its recorded subtree; a discount="12" fails assertion 1
 * (cvc-assertion) after its cast to type 56 succeeded.
 *
 * Storage
 * -------
 * Tables grow through the processor's allocator, doubling, while the
 * stages write, so pointers into a table are valid only until the next
 * addition to it; after sxml_schema_components_seal nothing grows. Blocks
 * (sxml_schema_components_allocate_block, glossary block) never move and
 * are released with the components; compiled XPath expressions of XPath
 * records are destroyed with them.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* ----------------------------------------------------------- Identifiers */

/* A type definition, simple or complex; 0 means none. 1 to 54 are the
   built-in types, by sxml_builtin_type. */
typedef struct sxml_schema_type_definition_index {
    uint32_t value;
} sxml_schema_type_definition_index;

typedef struct sxml_schema_element_declaration_index {
    uint32_t value;
} sxml_schema_element_declaration_index;

typedef struct sxml_schema_attribute_declaration_index {
    uint32_t value;
} sxml_schema_attribute_declaration_index;

typedef struct sxml_schema_attribute_use_index {
    uint32_t value;
} sxml_schema_attribute_use_index;

typedef struct sxml_schema_attribute_group_index {
    uint32_t value;
} sxml_schema_attribute_group_index;

typedef struct sxml_schema_model_group_definition_index {
    uint32_t value;
} sxml_schema_model_group_definition_index;

typedef struct sxml_schema_model_group_index {
    uint32_t value;
} sxml_schema_model_group_index;

typedef struct sxml_schema_particle_index {
    uint32_t value;
} sxml_schema_particle_index;

typedef struct sxml_schema_wildcard_index {
    uint32_t value;
} sxml_schema_wildcard_index;

typedef struct sxml_schema_identity_constraint_index {
    uint32_t value;
} sxml_schema_identity_constraint_index;

typedef struct sxml_schema_type_alternative_index {
    uint32_t value;
} sxml_schema_type_alternative_index;

typedef struct sxml_schema_assertion_index {
    uint32_t value;
} sxml_schema_assertion_index;

typedef struct sxml_schema_notation_declaration_index {
    uint32_t value;
} sxml_schema_notation_declaration_index;

typedef struct sxml_schema_facet_index {
    uint32_t value;
} sxml_schema_facet_index;

typedef struct sxml_schema_xpath_record_index {
    uint32_t value;
} sxml_schema_xpath_record_index;

typedef struct sxml_schema_namespace_context_index {
    uint32_t value;
} sxml_schema_namespace_context_index;

typedef struct sxml_schema_reference_index {
    uint32_t value;
} sxml_schema_reference_index;

/* A schema document; 0 for the built-in components. */
typedef struct sxml_schema_document_index {
    uint32_t value;
} sxml_schema_document_index;

/*
 * A run of indexes in the list table: entry_count entries from
 * first_entry_index. What the entries index is said where a list is a
 * field. An empty list is all zero. Lists are written once, whole
 * (sxml_schema_components_add_list), and later stages may rewrite their
 * entries in place.
 */
typedef struct sxml_schema_list {
    uint32_t first_entry_index;
    uint32_t entry_count;
} sxml_schema_list;

/* Bytes in the text table: byte_count bytes from byte_offset. */
typedef struct sxml_schema_text {
    uint32_t byte_offset;
    uint32_t byte_count;
} sxml_schema_text;

/* A run of namespace bindings in the namespace binding table. */
typedef struct sxml_schema_namespace_binding_list {
    uint32_t first_binding_index;
    uint32_t binding_count;
} sxml_schema_namespace_binding_list;

/* A list's entries, borrowed from the components. */
typedef struct sxml_schema_index_view {
    const uint32_t *indexes;
    size_t index_count;
} sxml_schema_index_view;

/* Where a component's XML representation starts (glossary
   schema_position); all zero for built-in components. */
typedef struct sxml_schema_position {
    sxml_schema_document_index document;
    uint32_t line_number;
    uint32_t column_number;
    size_t byte_offset;
} sxml_schema_position;

/* ----------------------------------------------------- Shared properties */

/* The symbol spaces of global components (2.6). */
typedef enum sxml_schema_symbol_space {
    SXML_SCHEMA_SYMBOL_SPACE_NONE = 0,
    SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
    SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
    SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
    SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION,
    SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP,
    SXML_SCHEMA_SYMBOL_SPACE_IDENTITY_CONSTRAINT,
    SXML_SCHEMA_SYMBOL_SPACE_NOTATION_DECLARATION,
} sxml_schema_symbol_space;

/*
 * Derivation methods, as bits of a derivation set (glossary
 * derivation_set: {final}, {prohibited substitutions}, {disallowed
 * substitutions}, {substitution group exclusions}), and one of them as
 * the {derivation method} of a type definition.
 */
typedef enum sxml_schema_derivation {
    SXML_SCHEMA_DERIVATION_NONE = 0,
    SXML_SCHEMA_DERIVATION_EXTENSION = 1U << 0U,
    SXML_SCHEMA_DERIVATION_RESTRICTION = 1U << 1U,
    SXML_SCHEMA_DERIVATION_LIST = 1U << 2U,
    SXML_SCHEMA_DERIVATION_UNION = 1U << 3U,
    SXML_SCHEMA_DERIVATION_SUBSTITUTION = 1U << 4U,
} sxml_schema_derivation;

/* A set of sxml_schema_derivation bits. */
typedef uint32_t sxml_schema_derivation_set;

/* The {variety} of a value constraint, or none. */
typedef enum sxml_schema_value_constraint_variety {
    SXML_SCHEMA_VALUE_CONSTRAINT_NONE = 0,
    SXML_SCHEMA_VALUE_CONSTRAINT_DEFAULT,
    SXML_SCHEMA_VALUE_CONSTRAINT_FIXED,
} sxml_schema_value_constraint_variety;

/*
 * A value constraint (3.2.1, 3.3.1, 3.5.1; glossary value_constraint).
 * XSD's {value} is the actual value of the lexical form in the
 * declaration's type; it is kept as the lexical form with its namespace
 * context (for xs:QName and xs:NOTATION values) and cast when needed,
 * after SIMPLE_TYPES (attributes) or COMPLEX_TYPES (elements) checked it.
 */
typedef struct sxml_schema_value_constraint {
    sxml_schema_value_constraint_variety variety;
    sxml_schema_text lexical_form;
    sxml_schema_namespace_context_index namespace_context;
} sxml_schema_value_constraint;

/* The {scope} {variety} of a declaration. */
typedef enum sxml_schema_scope_variety {
    SXML_SCHEMA_SCOPE_GLOBAL = 0,
    SXML_SCHEMA_SCOPE_LOCAL,
} sxml_schema_scope_variety;

/* The {mode} of open content; NONE when there is none. */
typedef enum sxml_schema_open_content_mode {
    SXML_SCHEMA_OPEN_CONTENT_NONE = 0,
    SXML_SCHEMA_OPEN_CONTENT_INTERLEAVE,
    SXML_SCHEMA_OPEN_CONTENT_SUFFIX,
} sxml_schema_open_content_mode;

/* A compiled content model (glossary content_model); defined in
   src/schema_content_model.c (5.6). */
typedef struct sxml_schema_content_model sxml_schema_content_model;

/* ----------------------------------------------------- Type definitions */

/*
 * A type definition (3.4.1 complex, 3.16.1 simple). variety is that of
 * types.h: COMPLEX for a complex type definition; ANY_SIMPLE (only
 * xs:anySimpleType), ATOMIC, LIST or UNION, the {variety}, for a simple
 * one. Fields marked "simple" or "complex" are meaningful only for that
 * kind; the others are zero.
 */
typedef struct sxml_schema_type_definition {
    sxml_type_variety variety;
    /* {name} and {target namespace}; 0 for an anonymous type. */
    sxml_name_identifier type_name;
    sxml_schema_position position;
    /* True for the registry's built-in types, types 1 to 54. */
    bool is_builtin;
    /* As read: base=; for a list or union, none (its base is
       xs:anySimpleType). */
    sxml_schema_reference_index base_type_reference;
    /* As read, simple and complex simple content: the anonymous
       xs:simpleType child of xs:restriction (simple content: of
       xs:simpleContent/xs:restriction), which is the base of a simple type
       and the base of the simple content type of a complex type. */
    sxml_schema_type_definition_index anonymous_base_type;
    /* Property, RESOLVE: {base type definition}; xs:anyType's is itself
       (3.4.7). */
    sxml_schema_type_definition_index base_type;
    /* As read: {derivation method} of a complex type (EXTENSION or
       RESTRICTION); for a simple type RESTRICTION, LIST or UNION, saying
       how it is made from its base. */
    sxml_schema_derivation derivation_method;
    /* As read, with finalDefault applied: {final}. */
    sxml_schema_derivation_set final_derivations;
    /* Property, SIMPLE_TYPES or COMPLEX_TYPES: the type's identifier in
       the processor's type registry; for built-in types (is_builtin),
       their sxml_builtin_type, from the start. */
    sxml_type_identifier registered_type;

    /* --- Simple type definitions (3.16.1) --- */
    /* Property, SIMPLE_TYPES: {primitive type definition} of an atomic
       type (0 for xs:anyAtomicType). */
    sxml_schema_type_definition_index primitive_type;
    /* As read: itemType=, or none. */
    sxml_schema_reference_index item_type_reference;
    /* As read: the anonymous xs:simpleType child of xs:list. */
    sxml_schema_type_definition_index anonymous_item_type;
    /* Property, RESOLVE: {item type definition} of a list type. */
    sxml_schema_type_definition_index item_type;
    /* As read: memberTypes=, a list of reference indexes, and the
       anonymous xs:simpleType children of xs:union, a list of type
       definition indexes. */
    sxml_schema_list member_type_references;
    sxml_schema_list anonymous_member_types;
    /* Property, RESOLVE: {member type definitions}, a list of type
       definition indexes: the referenced ones, then the anonymous ones
       (3.16.2.3). */
    sxml_schema_list member_types;
    /* As read: the constraining facets of the type's own restriction step
       (simple types; complex types: of their simpleContent restriction),
       a list of facet indexes in document order. Facets of base types are
       found through base_type. */
    sxml_schema_list facets;
    /* As read: own assertions (xs:assertion facets of a simple type,
       xs:assert of a complex type), a list of assertion indexes.
       Property, SIMPLE_TYPES or COMPLEX_TYPES: {assertions}, the base
       type's followed by the own ones (3.4.2.3.3, Part 2 4.3.13). */
    sxml_schema_list assertions;

    /* --- Complex type definitions (3.4.1) --- */
    /* As read: {abstract}. */
    bool is_abstract;
    /* As read, with blockDefault applied: {prohibited substitutions}. */
    sxml_schema_derivation_set prohibited_substitutions;
    /* As read: the effective mixed (3.4.2.3.1) and whether the content
       is xs:simpleContent. */
    bool is_mixed;
    bool has_simple_content;
    /* As read: the particle of the xs:sequence, xs:choice, xs:all or
       xs:group child (glossary explicit_content); 0 when there is none
       or it is empty (3.4.2.3.3, 2.1). */
    sxml_schema_particle_index explicit_content;
    /* As read: the xs:openContent child, else the schema document's
       xs:defaultOpenContent where it applies (3.4.2.3.3, 6). Property,
       COMPLEX_TYPES: the {open content} of the content type, 0 wildcard
       for none (mode NONE, or the absent wildcard of mode="none"). */
    sxml_schema_open_content_mode open_content_mode;
    sxml_schema_wildcard_index open_content_wildcard;
    /* As read: the type's own attribute uses (attribute use indexes,
       including prohibited ones), xs:attributeGroup references (reference
       indexes) and xs:anyAttribute (0: none). Property, COMPLEX_TYPES:
       attribute_uses and attribute_wildcard become {attribute uses} (no
       prohibited uses; the base's for extension and restriction as
       3.4.2.5 says; attribute groups and the default attribute group
       flattened) and {attribute wildcard} (3.4.2.6). */
    sxml_schema_list attribute_uses;
    sxml_schema_list attribute_group_references;
    sxml_schema_wildcard_index attribute_wildcard;
    /* As read: defaultAttributesApply and the schema document's
       defaultAttributes (none when either is absent or false). Property,
       RESOLVE: default_attribute_group. */
    bool applies_default_attributes;
    sxml_schema_reference_index default_attribute_group_reference;
    sxml_schema_attribute_group_index default_attribute_group;
    /* Property, COMPLEX_TYPES: {content type}: its variety, its particle
       (element-only and mixed) and its simple type definition
       (simple). */
    sxml_content_type_variety content_type_variety;
    sxml_schema_particle_index particle;
    sxml_schema_type_definition_index simple_content_type;
    /* Property, COMPLEX_TYPES: the compiled particle (element-only and
       mixed); NULL otherwise. In a block of the components. */
    const sxml_schema_content_model *content_model;
} sxml_schema_type_definition;

/* ---------------------------------------------------------- Declarations */

/* An element declaration (3.3.1). */
typedef struct sxml_schema_element_declaration {
    /* {name} and {target namespace}. */
    sxml_name_identifier element_name;
    sxml_schema_position position;
    /* As read: {scope}; for a local declaration its {parent}: the complex
       type or model group definition it is declared in (one of them). */
    sxml_schema_scope_variety scope_variety;
    sxml_schema_type_definition_index parent_type;
    sxml_schema_model_group_definition_index parent_group;
    /* As read: type=, and the anonymous type child. Property, RESOLVE:
       {type definition} (3.3.2.1 for a declaration with neither). */
    sxml_schema_reference_index type_reference;
    sxml_schema_type_definition_index anonymous_type;
    sxml_schema_type_definition_index type_definition;
    /* As read: {type table} {alternatives}, a list of type alternative
       indexes in document order; the last one has no test when the type
       table has an explicit default (3.3.2.2). */
    sxml_schema_list type_alternatives;
    /* As read: {value constraint}, {nillable}, {abstract}. */
    sxml_schema_value_constraint value_constraint;
    bool is_nillable;
    bool is_abstract;
    /* As read: {identity-constraint definitions}, a list of identity
       constraint indexes; RESOLVE replaces the entry of a ref= one by
       the definition it refers to. */
    sxml_schema_list identity_constraints;
    /* As read: substitutionGroup=, a list of reference indexes. Property,
       RESOLVE: {substitution group affiliations}, a list of element
       declaration indexes, and the members of this declaration's
       substitution group (glossary substitution_group): every
       declaration whose affiliations reach it, transitively, itself
       excluded, abstract ones included, in index order. */
    sxml_schema_list substitution_group_references;
    sxml_schema_list substitution_group_affiliations;
    sxml_schema_list substitution_group_members;
    /* As read, with finalDefault and blockDefault applied: {substitution
       group exclusions} (EXTENSION, RESTRICTION) and {disallowed
       substitutions} (SUBSTITUTION, EXTENSION, RESTRICTION). */
    sxml_schema_derivation_set substitution_group_exclusions;
    sxml_schema_derivation_set disallowed_substitutions;
} sxml_schema_element_declaration;

/* An attribute declaration (3.2.1). */
typedef struct sxml_schema_attribute_declaration {
    sxml_name_identifier attribute_name;
    sxml_schema_position position;
    /* As read: {scope}; for a local declaration its {parent}, a complex
       type or attribute group (one of them). */
    sxml_schema_scope_variety scope_variety;
    sxml_schema_type_definition_index parent_type;
    sxml_schema_attribute_group_index parent_attribute_group;
    /* As read: type= and the anonymous type child. Property, RESOLVE:
       {type definition}, xs:anySimpleType when neither is given. */
    sxml_schema_reference_index type_reference;
    sxml_schema_type_definition_index anonymous_type;
    sxml_schema_type_definition_index type_definition;
    /* As read: {value constraint} (none for a local declaration: its use
       has it, 3.2.2.2) and {inheritable}. */
    sxml_schema_value_constraint value_constraint;
    bool is_inheritable;
} sxml_schema_attribute_declaration;

/* The use= of an attribute use. PROHIBITED uses are kept as read, for
   restriction (3.4.2.5), and never in {attribute uses}. */
typedef enum sxml_schema_attribute_use_kind {
    SXML_SCHEMA_ATTRIBUTE_USE_OPTIONAL = 0,
    SXML_SCHEMA_ATTRIBUTE_USE_REQUIRED,
    SXML_SCHEMA_ATTRIBUTE_USE_PROHIBITED,
} sxml_schema_attribute_use_kind;

/* An attribute use (3.5.1): one xs:attribute in a complex type or
   attribute group. */
typedef struct sxml_schema_attribute_use {
    sxml_schema_position position;
    /* As read: use= ({required}). */
    sxml_schema_attribute_use_kind use_kind;
    /* As read: ref=, or the local declaration. Property, RESOLVE:
       {attribute declaration}. */
    sxml_schema_reference_index attribute_declaration_reference;
    sxml_schema_attribute_declaration_index local_attribute_declaration;
    sxml_schema_attribute_declaration_index attribute_declaration;
    /* As read: {value constraint} and {inheritable} (inheritable= of a
       ref, else the declaration's). */
    sxml_schema_value_constraint value_constraint;
    bool is_inheritable;
} sxml_schema_attribute_use;

/* An attribute group definition (3.6.1). */
typedef struct sxml_schema_attribute_group {
    sxml_name_identifier group_name;
    sxml_schema_position position;
    /* As read: own attribute uses, xs:attributeGroup references (reference
       indexes) and xs:anyAttribute. Property, COMPLEX_TYPES:
       attribute_uses and attribute_wildcard flattened over the referenced
       groups (3.6.2.2). */
    sxml_schema_list attribute_uses;
    sxml_schema_list attribute_group_references;
    sxml_schema_wildcard_index attribute_wildcard;
} sxml_schema_attribute_group;

/* A notation declaration (3.14.1). */
typedef struct sxml_schema_notation_declaration {
    sxml_name_identifier notation_name;
    sxml_schema_position position;
    bool has_system_identifier;
    sxml_schema_text system_identifier;
    bool has_public_identifier;
    sxml_schema_text public_identifier;
} sxml_schema_notation_declaration;

/* --------------------------------------------------------------- Content */

/* A model group definition (3.7.1): a name for a model group. */
typedef struct sxml_schema_model_group_definition {
    sxml_name_identifier group_name;
    sxml_schema_position position;
    sxml_schema_model_group_index model_group;
} sxml_schema_model_group_definition;

/* The {compositor} of a model group. */
typedef enum sxml_schema_compositor {
    SXML_SCHEMA_COMPOSITOR_SEQUENCE = 0,
    SXML_SCHEMA_COMPOSITOR_CHOICE,
    SXML_SCHEMA_COMPOSITOR_ALL,
} sxml_schema_compositor;

/* A model group (3.8.1). */
typedef struct sxml_schema_model_group {
    sxml_schema_compositor compositor;
    sxml_schema_position position;
    /* {particles}: a list of particle indexes, in order. */
    sxml_schema_list particles;
} sxml_schema_model_group;

/* maximum_occurrences of maxOccurs="unbounded". A count above
   UINT64_MAX - 1 is read as UINT64_MAX - 1, more than any tree has
   children. */
#define SXML_SCHEMA_UNBOUNDED UINT64_MAX

/* The kind of a particle's {term}. */
typedef enum sxml_schema_term_kind {
    SXML_SCHEMA_TERM_NONE = 0,
    SXML_SCHEMA_TERM_ELEMENT_DECLARATION,
    SXML_SCHEMA_TERM_MODEL_GROUP,
    SXML_SCHEMA_TERM_WILDCARD,
} sxml_schema_term_kind;

/* A particle (3.9.1). */
typedef struct sxml_schema_particle {
    sxml_schema_position position;
    /* {min occurs} and {max occurs}; SXML_SCHEMA_UNBOUNDED. */
    uint64_t minimum_occurrences;
    uint64_t maximum_occurrences;
    sxml_schema_term_kind term_kind;
    /* As read: ref= of an xs:element (an element declaration reference)
       or of an xs:group (a model group definition reference). */
    sxml_schema_reference_index term_reference;
    /* As read for local terms, property (RESOLVE) for references: the
       {term}, an element declaration, model group or schema wildcard
       index by term_kind (an xs:group reference gives the definition's
       model group). */
    uint32_t term;
} sxml_schema_particle;

/* The {namespace constraint} {variety} of a schema wildcard. */
typedef enum sxml_schema_namespace_constraint_variety {
    SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY = 0,
    SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION,
    SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT,
} sxml_schema_namespace_constraint_variety;

/* The {process contents} of a schema wildcard. */
typedef enum sxml_schema_process_contents {
    SXML_SCHEMA_PROCESS_CONTENTS_STRICT = 0,
    SXML_SCHEMA_PROCESS_CONTENTS_LAX,
    SXML_SCHEMA_PROCESS_CONTENTS_SKIP,
} sxml_schema_process_contents;

/* A schema wildcard (3.10.1; glossary schema_wildcard), of an element
   particle, an attribute wildcard or open content. */
typedef struct sxml_schema_wildcard {
    sxml_schema_position position;
    sxml_schema_namespace_constraint_variety namespace_constraint_variety;
    /* {namespaces}: namespace URI identifier indexes, 0 for absent. */
    sxml_schema_list namespaces;
    /* {disallowed names}: QNames as name identifier indexes, and the
       keywords defined and sibling. */
    sxml_schema_list disallowed_names;
    bool disallows_defined;
    bool disallows_sibling;
    sxml_schema_process_contents process_contents;
} sxml_schema_wildcard;

/* ------------------------------------------- Constraints and XPath */

/* The {identity-constraint category}. */
typedef enum sxml_schema_identity_constraint_category {
    SXML_SCHEMA_IDENTITY_CONSTRAINT_UNIQUE = 0,
    SXML_SCHEMA_IDENTITY_CONSTRAINT_KEY,
    SXML_SCHEMA_IDENTITY_CONSTRAINT_KEYREF,
} sxml_schema_identity_constraint_category;

/* An identity-constraint definition (3.11.1). */
typedef struct sxml_schema_identity_constraint {
    /* {name} and {target namespace}; 0 for a ref= one. */
    sxml_name_identifier constraint_name;
    sxml_schema_position position;
    sxml_schema_identity_constraint_category category;
    /* As read: ref= (XSD 1.1, 3.11.2): the definition this element stands
       for; RESOLVE replaces it in its element's list. */
    sxml_schema_reference_index definition_reference;
    /* As read: {selector} and {fields} (a list of XPath record
       indexes). */
    sxml_schema_xpath_record_index selector;
    sxml_schema_list fields;
    /* As read: refer=. Property, RESOLVE: {referenced key} of a keyref. */
    sxml_schema_reference_index referenced_key_reference;
    sxml_schema_identity_constraint_index referenced_key;
} sxml_schema_identity_constraint;

/* A type alternative (3.12.1). */
typedef struct sxml_schema_type_alternative {
    sxml_schema_position position;
    /* {test}; 0 when it has none. */
    sxml_schema_xpath_record_index test;
    /* As read: type= and the anonymous type child. Property, RESOLVE:
       {type definition}. */
    sxml_schema_reference_index type_reference;
    sxml_schema_type_definition_index anonymous_type;
    sxml_schema_type_definition_index type_definition;
} sxml_schema_type_alternative;

/* An assertion (3.13.1): xs:assert or an xs:assertion facet. */
typedef struct sxml_schema_assertion {
    sxml_schema_position position;
    sxml_schema_xpath_record_index test;
} sxml_schema_assertion;

/* A constraining facet of a restriction step (Part 2, 4.3), as read.
   Assertion facets are assertions, not facets (types.h). */
typedef struct sxml_schema_facet {
    sxml_facet_kind facet_kind;
    sxml_schema_position position;
    /* The value attribute as written; QName and NOTATION values are read
       with namespace_context. */
    sxml_schema_text value;
    sxml_schema_namespace_context_index namespace_context;
    bool is_fixed;
} sxml_schema_facet;

/*
 * An XPath expression record (3.13.1; glossary xpath_record): the test of
 * an assertion or type alternative, a selector or field.
 */
typedef struct sxml_schema_xpath_record {
    sxml_schema_position position;
    /* {namespace bindings}, {default namespace}, {base URI}. */
    sxml_schema_namespace_context_index namespace_context;
    /* {expression}, as written. */
    sxml_schema_text expression;
    /* Property, IDENTITY_CONSTRAINTS or ASSERTIONS: the expression
       compiled for the processor; owned, destroyed with the components.
       NULL before. */
    sxml_xpath_expression *compiled_expression;
} sxml_schema_xpath_record;

/*
 * The namespaces of an element of a schema document (glossary
 * namespace_context): what QName values (facets, value constraints) and
 * XPath records are read with. The reader adds one when they differ from
 * the last one added.
 */
typedef struct sxml_schema_namespace_context {
    /* The in-scope namespaces. */
    sxml_schema_namespace_binding_list namespace_bindings;
    /* xpathDefaultNamespace resolved (##targetNamespace,
       ##defaultNamespace, ##local); SXML_NAMESPACE_NONE for ##local or
       absent. */
    sxml_namespace_uri_identifier default_namespace;
    /* The base URI (xml:base resolved against the document's URI). */
    sxml_schema_text base_uri;
} sxml_schema_namespace_context;

/* A schema reference (glossary schema_reference). */
typedef struct sxml_schema_reference {
    sxml_schema_symbol_space symbol_space;
    sxml_name_identifier name;
    sxml_schema_position position;
    /* Property, RESOLVE: the component's index in the table of its
       symbol space; 0 when it does not resolve (src-resolve reported).
       The reader may make a reference resolved at once (a redefinition's
       reference to the original). */
    uint32_t component;
} sxml_schema_reference;

/* A schema document the schema was assembled from. */
typedef struct sxml_schema_document_entry {
    /* The absolute URI; may be empty. */
    sxml_schema_text document_uri;
    /* The document_uri of the sxml_schema_document the configuration
       gave, for a document it names directly: borrowed from the
       configuration, as the error record's source name; empty for the
       others. Not valid after compilation. */
    sxml_byte_view configured_uri;
    /* targetNamespace (after chameleon inclusion). */
    sxml_namespace_uri_identifier target_namespace;
} sxml_schema_document_entry;

/* ---------------------------------------------------------- Components */

/* The tables of the components (glossary schema_table). */
typedef enum sxml_schema_table {
    SXML_SCHEMA_TABLE_TYPE_DEFINITION = 0,
    SXML_SCHEMA_TABLE_ELEMENT_DECLARATION,
    SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION,
    SXML_SCHEMA_TABLE_ATTRIBUTE_USE,
    SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP,
    SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION,
    SXML_SCHEMA_TABLE_MODEL_GROUP,
    SXML_SCHEMA_TABLE_PARTICLE,
    SXML_SCHEMA_TABLE_WILDCARD,
    SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT,
    SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE,
    SXML_SCHEMA_TABLE_ASSERTION,
    SXML_SCHEMA_TABLE_NOTATION_DECLARATION,
    SXML_SCHEMA_TABLE_FACET,
    SXML_SCHEMA_TABLE_XPATH_RECORD,
    SXML_SCHEMA_TABLE_NAMESPACE_CONTEXT,
    SXML_SCHEMA_TABLE_REFERENCE,
    SXML_SCHEMA_TABLE_DOCUMENT,
    /* uint32_t entries of lists. */
    SXML_SCHEMA_TABLE_LIST_ENTRY,
    /* sxml_namespace_binding entries. */
    SXML_SCHEMA_TABLE_NAMESPACE_BINDING,
    /* Bytes of texts. */
    SXML_SCHEMA_TABLE_TEXT_BYTE,
    SXML_SCHEMA_TABLE_COUNT,
} sxml_schema_table;

/* One table: entry_count entries (0, or at least 1 with entry 0
   reserved) of the table's entry type. */
typedef struct sxml_schema_table_storage {
    void *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;
} sxml_schema_table_storage;

/* One entry of the name index (glossary schema_name_index). */
typedef struct sxml_schema_name_entry {
    sxml_schema_symbol_space symbol_space;
    sxml_name_identifier name;
    /* The index in the symbol space's table; 0 marks a free slot. */
    uint32_t component;
} sxml_schema_name_entry;

typedef struct sxml_schema_components_configuration {
    /* Kept by reference; must outlive the components. Its allocator gives
       the tables and blocks, its name pool the names. */
    sxml_processor *processor;
} sxml_schema_components_configuration;

/*
 * The components of one schema (glossary schema_components). The layout
 * is visible for the inline accessors below; only the functions of this
 * header write it.
 */
typedef struct sxml_schema_components {
    sxml_processor *processor;
    /* The processor's allocator: tables and blocks. */
    sxml_allocator allocator;
    /* The allocator of sxml_schema_components_create, which destroy frees
       the components with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    bool is_sealed;
    sxml_schema_table_storage tables[SXML_SCHEMA_TABLE_COUNT];
    /* Open addressing, a power of two slots, at most half in use. */
    sxml_schema_name_entry *name_entries;
    uint32_t name_entry_count;
    uint32_t name_slot_count;
    /* The blocks of sxml_schema_components_allocate_block, linked through
       their headers. */
    void *last_block;
} sxml_schema_components;

/*
 * The functions below are defined in src/schema_components.c (packet
 * 5.2). They follow the conventions of sxml/memory.h; on failure outputs
 * and the components are unchanged (D20). Additions return
 * SXML_STATUS_INVALID_ARGUMENT on sealed components,
 * SXML_STATUS_LIMIT_EXCEEDED past UINT32_MAX - 1 entries of a table, and
 * the allocator's failures.
 */

/* Writes the requirements of the components' fixed part to
   *requirements. SXML_STATUS_INVALID_ARGUMENT without a processor. */
SXML_NODISCARD sxml_status sxml_schema_components_memory_requirements(
    const sxml_schema_components_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places empty components (no tables yet; no built-in components) in
   memory, which must stay valid, unmoved, until finalized. Allocates
   nothing. */
SXML_NODISCARD sxml_status sxml_schema_components_initialize(
    sxml_memory_view memory,
    const sxml_schema_components_configuration *configuration,
    sxml_schema_components **components);

/* Destroys the compiled XPath expressions and releases the tables and
   blocks; accepts NULL. Type definitions stay registered. */
void sxml_schema_components_finalize(sxml_schema_components *components);

/* Allocates through allocator and initializes; allocator is stored and
   must outlive the components. */
SXML_NODISCARD sxml_status sxml_schema_components_create(
    sxml_allocator allocator,
    const sxml_schema_components_configuration *configuration,
    sxml_schema_components **components);

/* Finalizes components made by sxml_schema_components_create and frees
   them; accepts NULL. */
void sxml_schema_components_destroy(sxml_schema_components *components);

/*
 * Adds the built-in components (see the top of this header) to empty
 * components and enters their names. Returns
 * SXML_STATUS_INVALID_ARGUMENT when a table is not empty, and the
 * failures of the additions and of the name pool.
 */
SXML_NODISCARD sxml_status sxml_schema_components_add_builtin_components(
    sxml_schema_components *components);

/* Stops all growth: pointers into the components stay valid for their
   lifetime. */
void sxml_schema_components_seal(sxml_schema_components *components);

/* Entries to append to one table. */
typedef struct sxml_schema_entry_request {
    sxml_schema_table table;
    /* entry_count entries of the table's entry type, copied. */
    const void *entries;
    uint32_t entry_count;
} sxml_schema_entry_request;

/*
 * Appends request->entries to request->table (reserving entry 0 on the
 * table's first addition) and writes the index of the first one to
 * *first_index. The typed index of a component is {.value =
 * *first_index}. Returns SXML_STATUS_INVALID_ARGUMENT for an entry_count
 * of 0.
 */
SXML_NODISCARD sxml_status sxml_schema_components_add_entries(
    sxml_schema_components *components,
    const sxml_schema_entry_request *request, uint32_t *first_index);

/* An entry to change. */
typedef struct sxml_schema_entry_key {
    sxml_schema_table table;
    uint32_t index;
} sxml_schema_entry_key;

/*
 * The entry at key, for a stage to write its properties: a pointer of the
 * table's entry type, valid until the next addition to that table; NULL
 * for index 0, an index not in use, or sealed components.
 */
void *sxml_schema_components_edit_entry(sxml_schema_components *components,
                                        sxml_schema_entry_key key);

/* Copies indexes into the list table and writes the run to *list; an
   empty view gives the empty list without adding anything. */
SXML_NODISCARD sxml_status sxml_schema_components_add_list(
    sxml_schema_components *components, sxml_schema_index_view indexes,
    sxml_schema_list *list);

/* Copies bytes into the text table and writes the run to *text. */
SXML_NODISCARD sxml_status sxml_schema_components_add_text(
    sxml_schema_components *components, sxml_byte_view bytes,
    sxml_schema_text *text);

/* Copies bindings into the namespace binding table and writes the run to
   *list. */
SXML_NODISCARD sxml_status sxml_schema_components_add_namespace_bindings(
    sxml_schema_components *components, sxml_namespace_binding_view bindings,
    sxml_schema_namespace_binding_list *list);

/*
 * Enters entry in the name index. When the symbol space already has a
 * component of that name, nothing changes and its index is written to
 * *existing_component (the caller reports the duplicate,
 * sch-props-correct.2); otherwise 0 is written. Returns
 * SXML_STATUS_INVALID_ARGUMENT for symbol space NONE, name 0 or component
 * 0.
 */
SXML_NODISCARD sxml_status sxml_schema_components_enter_name(
    sxml_schema_components *components, const sxml_schema_name_entry *entry,
    uint32_t *existing_component);

/* Makes the name of entry stand for entry->component (xs:redefine,
   xs:override). Returns SXML_STATUS_INVALID_ARGUMENT when the name was
   not entered. */
SXML_NODISCARD sxml_status sxml_schema_components_replace_name(
    sxml_schema_components *components, const sxml_schema_name_entry *entry);

/* What to look up in the name index. */
typedef struct sxml_schema_name_key {
    sxml_schema_symbol_space symbol_space;
    sxml_name_identifier name;
} sxml_schema_name_key;

/* Writes the index of the global component key names to *component and
   returns true; false, leaving *component untouched, when there is none.
   O(1) average. */
bool sxml_schema_components_find_name(const sxml_schema_components *components,
                                      sxml_schema_name_key key,
                                      uint32_t *component);

/* Allocates a block that never moves and lives until the components are
   finalized (content models). */
SXML_NODISCARD sxml_status sxml_schema_components_allocate_block(
    sxml_schema_components *components, sxml_allocation_request request,
    void **memory);

/*
 * Writes the text form (see the top of this header) into output and the
 * bytes it needs to *byte_count; as sxml_xslt_ast_write_text,
 * *byte_count is written on SXML_STATUS_CAPACITY_EXCEEDED too and output
 * then holds a prefix. src/schema_text.c (5.4).
 */
SXML_NODISCARD sxml_status sxml_schema_components_write_text(
    const sxml_schema_components *components, sxml_memory_view output,
    size_t *byte_count);

/* ------------------------------------------------------------- Accessors */

/* The entry at key, or NULL for 0 and out of range. */
static inline const void *sxml_schema_components_find_entry(
    const sxml_schema_components *components, sxml_schema_entry_key key,
    size_t entry_byte_count) {
    const sxml_schema_table_storage *storage = &components->tables[key.table];
    if (key.index == 0 || key.index >= storage->entry_count) {
        return NULL;
    }
    return (const uint8_t *)storage->entries + (size_t)key.index * entry_byte_count;
}

/* Each accessor returns the component at index, borrowed (valid until the
   next addition to its table), or a zero entry for 0 and out of range. */

static inline const sxml_schema_type_definition *sxml_schema_get_type_definition(
    const sxml_schema_components *components,
    sxml_schema_type_definition_index index) {
    static const sxml_schema_type_definition no_entry;
    const sxml_schema_type_definition *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                             .index = index.value},
        sizeof(sxml_schema_type_definition));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_element_declaration *
sxml_schema_get_element_declaration(const sxml_schema_components *components,
                                    sxml_schema_element_declaration_index index) {
    static const sxml_schema_element_declaration no_entry;
    const sxml_schema_element_declaration *entry =
        sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ELEMENT_DECLARATION,
                                             .index = index.value},
        sizeof(sxml_schema_element_declaration));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_attribute_declaration *
sxml_schema_get_attribute_declaration(
    const sxml_schema_components *components,
    sxml_schema_attribute_declaration_index index) {
    static const sxml_schema_attribute_declaration no_entry;
    const sxml_schema_attribute_declaration *entry =
        sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION,
                                             .index = index.value},
        sizeof(sxml_schema_attribute_declaration));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_attribute_use *sxml_schema_get_attribute_use(
    const sxml_schema_components *components,
    sxml_schema_attribute_use_index index) {
    static const sxml_schema_attribute_use no_entry;
    const sxml_schema_attribute_use *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_USE,
                                             .index = index.value},
        sizeof(sxml_schema_attribute_use));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_attribute_group *sxml_schema_get_attribute_group(
    const sxml_schema_components *components,
    sxml_schema_attribute_group_index index) {
    static const sxml_schema_attribute_group no_entry;
    const sxml_schema_attribute_group *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP,
                                             .index = index.value},
        sizeof(sxml_schema_attribute_group));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_model_group_definition *
sxml_schema_get_model_group_definition(
    const sxml_schema_components *components,
    sxml_schema_model_group_definition_index index) {
    static const sxml_schema_model_group_definition no_entry;
    const sxml_schema_model_group_definition *entry =
        sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION,
                                             .index = index.value},
        sizeof(sxml_schema_model_group_definition));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_model_group *sxml_schema_get_model_group(
    const sxml_schema_components *components,
    sxml_schema_model_group_index index) {
    static const sxml_schema_model_group no_entry;
    const sxml_schema_model_group *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_MODEL_GROUP,
                                             .index = index.value},
        sizeof(sxml_schema_model_group));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_particle *sxml_schema_get_particle(
    const sxml_schema_components *components, sxml_schema_particle_index index) {
    static const sxml_schema_particle no_entry;
    const sxml_schema_particle *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_PARTICLE,
                                             .index = index.value},
        sizeof(sxml_schema_particle));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_wildcard *sxml_schema_get_wildcard(
    const sxml_schema_components *components, sxml_schema_wildcard_index index) {
    static const sxml_schema_wildcard no_entry;
    const sxml_schema_wildcard *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_WILDCARD,
                                             .index = index.value},
        sizeof(sxml_schema_wildcard));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_identity_constraint *
sxml_schema_get_identity_constraint(const sxml_schema_components *components,
                                    sxml_schema_identity_constraint_index index) {
    static const sxml_schema_identity_constraint no_entry;
    const sxml_schema_identity_constraint *entry =
        sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT,
                                             .index = index.value},
        sizeof(sxml_schema_identity_constraint));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_type_alternative *
sxml_schema_get_type_alternative(const sxml_schema_components *components,
                                 sxml_schema_type_alternative_index index) {
    static const sxml_schema_type_alternative no_entry;
    const sxml_schema_type_alternative *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE,
                                             .index = index.value},
        sizeof(sxml_schema_type_alternative));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_assertion *sxml_schema_get_assertion(
    const sxml_schema_components *components,
    sxml_schema_assertion_index index) {
    static const sxml_schema_assertion no_entry;
    const sxml_schema_assertion *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ASSERTION,
                                             .index = index.value},
        sizeof(sxml_schema_assertion));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_notation_declaration *
sxml_schema_get_notation_declaration(
    const sxml_schema_components *components,
    sxml_schema_notation_declaration_index index) {
    static const sxml_schema_notation_declaration no_entry;
    const sxml_schema_notation_declaration *entry =
        sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_NOTATION_DECLARATION,
                                             .index = index.value},
        sizeof(sxml_schema_notation_declaration));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_facet *sxml_schema_get_facet(
    const sxml_schema_components *components, sxml_schema_facet_index index) {
    static const sxml_schema_facet no_entry;
    const sxml_schema_facet *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_FACET,
                                             .index = index.value},
        sizeof(sxml_schema_facet));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_xpath_record *sxml_schema_get_xpath_record(
    const sxml_schema_components *components,
    sxml_schema_xpath_record_index index) {
    static const sxml_schema_xpath_record no_entry;
    const sxml_schema_xpath_record *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_XPATH_RECORD,
                                             .index = index.value},
        sizeof(sxml_schema_xpath_record));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_namespace_context *
sxml_schema_get_namespace_context(const sxml_schema_components *components,
                                  sxml_schema_namespace_context_index index) {
    static const sxml_schema_namespace_context no_entry;
    const sxml_schema_namespace_context *entry =
        sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_NAMESPACE_CONTEXT,
                                             .index = index.value},
        sizeof(sxml_schema_namespace_context));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_reference *sxml_schema_get_reference(
    const sxml_schema_components *components,
    sxml_schema_reference_index index) {
    static const sxml_schema_reference no_entry;
    const sxml_schema_reference *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_REFERENCE,
                                             .index = index.value},
        sizeof(sxml_schema_reference));
    return entry != NULL ? entry : &no_entry;
}

static inline const sxml_schema_document_entry *sxml_schema_get_document(
    const sxml_schema_components *components, sxml_schema_document_index index) {
    static const sxml_schema_document_entry no_entry;
    const sxml_schema_document_entry *entry = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_DOCUMENT,
                                             .index = index.value},
        sizeof(sxml_schema_document_entry));
    return entry != NULL ? entry : &no_entry;
}

/* The entries of list; empty for the empty list. */
static inline sxml_schema_index_view sxml_schema_get_list(
    const sxml_schema_components *components, sxml_schema_list list) {
    const uint32_t *entries = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_LIST_ENTRY,
                                             .index = list.first_entry_index},
        sizeof(uint32_t));
    if (entries == NULL || list.entry_count == 0) {
        return (sxml_schema_index_view){.indexes = NULL, .index_count = 0};
    }
    return (sxml_schema_index_view){.indexes = entries,
                                    .index_count = list.entry_count};
}

/* The bytes of text; empty for the empty text. */
static inline sxml_byte_view sxml_schema_get_text(
    const sxml_schema_components *components, sxml_schema_text text) {
    const uint8_t *bytes = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TEXT_BYTE,
                                             .index = text.byte_offset},
        1);
    if (bytes == NULL || text.byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = bytes, .byte_count = text.byte_count};
}

/* The bindings of list; empty for the empty list. */
static inline sxml_namespace_binding_view sxml_schema_get_namespace_bindings(
    const sxml_schema_components *components,
    sxml_schema_namespace_binding_list list) {
    const sxml_namespace_binding *bindings = sxml_schema_components_find_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_NAMESPACE_BINDING,
                                             .index = list.first_binding_index},
        sizeof(sxml_namespace_binding));
    if (bindings == NULL || list.binding_count == 0) {
        return (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    }
    return (sxml_namespace_binding_view){
        .namespace_bindings = bindings,
        .namespace_binding_count = list.binding_count};
}

/* ---------------------------------------------------------------- Schema */

/*
 * A compiled schema (include/sxml/schema.h). The quintet, the find
 * functions and the run of the compile stages are src/schema.c's (packet
 * 5.4); the layout is here so that every src/schema*.c file reaches the
 * components.
 */
struct sxml_schema {
    sxml_processor *processor;
    /* The allocator of sxml_schema_create, which destroy frees the schema
       with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    sxml_schema_components components;
};

/* ---------------------------------------------------------- Compilation */

/* What one compilation passes from stage to stage (glossary
   schema_compilation). Lives for one sxml_schema_initialize. */
typedef struct sxml_schema_compilation {
    /* Borrowed for the compilation. */
    const sxml_schema_configuration *configuration;
    sxml_schema_components *components;
    /* Errors reported so far. */
    size_t error_count;
} sxml_schema_compilation;

/* An error a stage found. */
typedef struct sxml_schema_report_request {
    /* The constraint name (glossary constraint_name), static text. */
    sxml_byte_view constraint_name;
    /* Static text: the description of the error record. */
    sxml_byte_view description;
    /* What the report sink receives as the description when not empty:
       the description with the names involved. Borrowed for the call. */
    sxml_byte_view detailed_description;
    sxml_schema_position position;
} sxml_schema_report_request;

/*
 * Reports an error: counts it, fills the configuration's error record
 * with the first one (code the constraint name in no namespace, the
 * static description, the document's configured URI as source name) and
 * gives it to the configuration's report sink. Returns SXML_STATUS_OK
 * when the stage may go on (the sink accepted it), the sink's status when
 * it stopped, and SXML_STATUS_STATIC_ERROR with the null sink. Defined in
 * src/schema_components.c.
 */
SXML_NODISCARD sxml_status sxml_schema_compilation_report(
    sxml_schema_compilation *compilation,
    const sxml_schema_report_request *request);

/* The stages, in the order sxml_schema_initialize runs them (see the top
   of this header). Each returns the failures of what it calls,
   SXML_STATUS_LIMIT_EXCEEDED at a limit, and SXML_STATUS_OK otherwise,
   also when it reported errors. */

/* READ: src/schema_read.c (5.4). */
SXML_NODISCARD sxml_status sxml_schema_read_documents(
    sxml_schema_compilation *compilation);
/* RESOLVE: src/schema_resolve.c (5.4). */
SXML_NODISCARD sxml_status sxml_schema_resolve_references(
    sxml_schema_compilation *compilation);
/* SIMPLE_TYPES: src/schema_simple_type.c (5.5). */
SXML_NODISCARD sxml_status sxml_schema_compile_simple_types(
    sxml_schema_compilation *compilation);
/* COMPLEX_TYPES: src/schema_complex_type.c (5.6). */
SXML_NODISCARD sxml_status sxml_schema_compile_complex_types(
    sxml_schema_compilation *compilation);
/* IDENTITY_CONSTRAINTS: src/schema_identity.c (5.8). */
SXML_NODISCARD sxml_status sxml_schema_compile_identity_constraints(
    sxml_schema_compilation *compilation);
/* ASSERTIONS: src/schema_assertion.c (5.9). */
SXML_NODISCARD sxml_status sxml_schema_compile_assertions(
    sxml_schema_compilation *compilation);

/*
 * Checks and registers one simple type definition whose base is
 * registered, as SIMPLE_TYPES does for each: for the simple content types
 * COMPLEX_TYPES synthesizes (3.4.2.2). src/schema_simple_type.c (5.5).
 */
SXML_NODISCARD sxml_status sxml_schema_compile_simple_type(
    sxml_schema_compilation *compilation,
    sxml_schema_type_definition_index simple_type);

/* A derivation question (cos-ct-derived-ok, cos-st-derived-ok,
   3.4.6.5 and 3.16.6.3). */
typedef struct sxml_schema_derivation_request {
    sxml_schema_type_definition_index derived_type;
    sxml_schema_type_definition_index base_type;
    /* Derivation methods that may not be used on the way (the blocking
       of xsi:type and substitution groups, cvc-elt.4.3, 3.3.6.3). */
    sxml_schema_derivation_set blocked_derivations;
} sxml_schema_derivation_request;

/* The answer to a derivation question. */
typedef struct sxml_schema_derivation_outcome {
    bool is_derived;
} sxml_schema_derivation_outcome;

/*
 * Writes to outcome->is_derived whether request->derived_type is validly derived
 * from request->base_type without a blocked method (for COMPLEX_TYPES'
 * substitution groups and type tables, the validator's xsi:type, and
 * schema-aware XPath). src/schema_complex_type.c (5.6); its stub returns
 * SXML_STATUS_UNSUPPORTED_FEATURE.
 */
SXML_NODISCARD sxml_status sxml_schema_check_derivation(
    const sxml_schema_components *components,
    const sxml_schema_derivation_request *request,
    sxml_schema_derivation_outcome *outcome);

/* ------------------------------------------------------ Content models */

/*
 * Compiles the particle of complex_type (element-only or mixed content,
 * properties written) into a content model and writes it to the type's
 * content_model, checking cos-nonambig and the state limit
 * (maximum_content_model_state_count; SXML_STATUS_LIMIT_EXCEEDED).
 * Called by COMPLEX_TYPES. src/schema_content_model.c (5.6).
 */
SXML_NODISCARD sxml_status sxml_schema_compile_content_model(
    sxml_schema_compilation *compilation,
    sxml_schema_type_definition_index complex_type);

/* Where matching has reached (glossary content_model_cursor). The caller
   owns counter_values, room for counter_count values, which is what
   sxml_schema_content_model_get_counter_count says. */
typedef struct sxml_schema_content_model_cursor {
    const sxml_schema_content_model *content_model;
    uint32_t state;
    uint64_t *counter_values;
    uint32_t counter_count;
} sxml_schema_content_model_cursor;

/* The counters a cursor of content_model needs. */
uint32_t sxml_schema_content_model_get_counter_count(
    const sxml_schema_content_model *content_model);

/* Puts cursor (content_model, counter_values and counter_count set) at
   the start state, its counters 0. */
void sxml_schema_content_model_start(sxml_schema_content_model_cursor *cursor);

/* What a child element matched. */
typedef enum sxml_schema_match_kind {
    /* Not allowed here (cvc-complex-type.2.4). */
    SXML_SCHEMA_MATCH_NONE = 0,
    SXML_SCHEMA_MATCH_ELEMENT_DECLARATION,
    SXML_SCHEMA_MATCH_WILDCARD,
} sxml_schema_match_kind;

typedef struct sxml_schema_content_model_match {
    sxml_schema_match_kind match_kind;
    /* ELEMENT_DECLARATION: the declaration whose name matched, a member
       of the transition's substitution group when that matched. */
    sxml_schema_element_declaration_index element_declaration;
    /* WILDCARD: the schema wildcard, of a particle or of open content. */
    sxml_schema_wildcard_index wildcard;
} sxml_schema_content_model_match;

/* One child element to match. */
typedef struct sxml_schema_content_model_step_request {
    /* In/out: moved on when the child matches, unchanged otherwise. */
    sxml_schema_content_model_cursor *cursor;
    sxml_name_identifier element_name;
} sxml_schema_content_model_step_request;

/* Matches one child element and writes what it matched to *match.
   Returns the name pool's failures. src/schema_content_model.c (5.6). */
SXML_NODISCARD sxml_status sxml_schema_content_model_step(
    const sxml_schema_components *components,
    const sxml_schema_content_model_step_request *request,
    sxml_schema_content_model_match *match);

/* True when the content may end where cursor is (its state is final and
   its counters satisfy the minimums). */
bool sxml_schema_content_model_can_end(
    const sxml_schema_content_model_cursor *cursor);

/* Room for expected terms (glossary expected_term): each an element
   declaration or schema wildcard match. */
typedef struct sxml_schema_expected_term_buffer {
    sxml_schema_content_model_match *terms;
    size_t term_capacity;
    /* Out: the terms written. */
    size_t term_count;
} sxml_schema_expected_term_buffer;

/* Writes the terms a step from cursor would accept, at most
   term_capacity of them, in transition order (for error messages). */
void sxml_schema_content_model_list_expected(
    const sxml_schema_components *components,
    const sxml_schema_content_model_cursor *cursor,
    sxml_schema_expected_term_buffer *buffer);

/* A name to test against a schema wildcard (3.10.4.1, cvc-wildcard). */
typedef struct sxml_schema_wildcard_test {
    sxml_schema_wildcard_index wildcard;
    sxml_name_identifier name;
    /* ELEMENT_DECLARATION or ATTRIBUTE_DECLARATION: where the keyword
       defined looks for a global declaration of the name. */
    sxml_schema_symbol_space symbol_space;
} sxml_schema_wildcard_test;

/*
 * True when the wildcard's namespace constraint allows the name and its
 * disallowed names do not exclude it (the keyword sibling is checked by
 * the content model, which knows the siblings).
 * src/schema_content_model.c (5.6).
 */
bool sxml_schema_wildcard_allows_name(const sxml_schema_components *components,
                                      const sxml_schema_wildcard_test *test);

/* ------------------------------------------------- Validator seams (5.7) */

/*
 * The recorded subtree of an element (glossary recorded_subtree): the
 * validated copy the validator keeps while the element is open, built
 * from the events it sends, so its nodes carry their type annotations
 * and typed values derive from them (D165).
 */
typedef struct sxml_schema_recorded_subtree {
    /* The element, in an arena tree the validator owns; borrowed. */
    sxml_item element_node;
    /* The governing element declaration of each element node of that
       tree, by node index (0 for one assessed laxly without one). */
    const sxml_schema_element_declaration_index *element_declarations;
    size_t element_declaration_count;
} sxml_schema_recorded_subtree;

/*
 * Reports a validation error to the validator's report sink, as
 * include/sxml/schema.h's validator operations do, and returns their
 * status. src/schema_validate.c (5.7); its stub returns
 * SXML_STATUS_UNSUPPORTED_FEATURE.
 */
SXML_NODISCARD sxml_status sxml_schema_validator_report(
    sxml_schema_validator *validator, const sxml_schema_report *report);

/* The IDs and IDREFs of one validation root (glossary id_table);
   defined in src/schema_validate.c (5.7). */
typedef struct sxml_schema_id_table sxml_schema_id_table;

/* One ID or IDREF value met. */
typedef struct sxml_schema_id_request {
    /* The value, as cast to xs:ID or xs:IDREF; copied. */
    sxml_byte_view value;
    /* Where it was met, for cvc-id.1 and cvc-id.2 reports. */
    sxml_source_location location;
} sxml_schema_id_request;

/* What recording an ID found. */
typedef struct sxml_schema_id_outcome {
    /* The ID was met before under this validation root (cvc-id.2). */
    bool is_duplicate;
} sxml_schema_id_outcome;

/* Records an ID and writes what it found to *outcome.
   src/schema_validate.c (5.7). */
SXML_NODISCARD sxml_status sxml_schema_id_table_add_id(
    sxml_schema_id_table *id_table, const sxml_schema_id_request *request,
    sxml_schema_id_outcome *outcome);

/* Records an IDREF to check at the end of the validation root.
   src/schema_validate.c (5.7). */
SXML_NODISCARD sxml_status sxml_schema_id_table_add_idref(
    sxml_schema_id_table *id_table, const sxml_schema_id_request *request);

/* Reports every IDREF without its ID (cvc-id.1) through validator.
   src/schema_validate.c (5.7). */
SXML_NODISCARD sxml_status sxml_schema_id_table_check_idrefs(
    sxml_schema_id_table *id_table, sxml_schema_validator *validator);

/* ------------------------------------------- Assertions, alternatives,
   identity constraints (5.8, 5.9) */

/* Type alternatives to choose from (3.12.4). */
typedef struct sxml_schema_type_alternative_request {
    const sxml_schema *schema;
    sxml_schema_element_declaration_index element_declaration;
    /* A parentless copy of the element with its attributes and the
       attributes it inherits, no children (3.3.4.1, 3.12.4); borrowed. */
    sxml_item element_node;
} sxml_schema_type_alternative_request;

/*
 * Writes the type definition the declaration's type table selects for
 * the element (its declared type when the table is empty) to
 * *selected_type. A test that raises a dynamic error does not select its
 * type (3.12.4). src/schema_assertion.c (5.9); its stub handles an empty
 * table and returns SXML_STATUS_UNSUPPORTED_FEATURE for any other.
 */
SXML_NODISCARD sxml_status sxml_schema_select_type_alternative(
    sxml_schema_validator *validator,
    const sxml_schema_type_alternative_request *request,
    sxml_schema_type_definition_index *selected_type);

/* Assertions to check (3.13.4, Part 2 4.3.13). */
typedef struct sxml_schema_assertion_request {
    const sxml_schema *schema;
    /* The type whose {assertions} apply. */
    sxml_schema_type_definition_index type_definition;
    /* Complex types: the element of its recorded subtree. Simple types:
       the element or attribute, or none for a value alone. */
    sxml_item node;
    /* Simple types and simple content: the typed value, $value. */
    sxml_sequence value;
} sxml_schema_assertion_request;

/*
 * Evaluates each assertion of the type and reports each false one or one
 * raising an error (cvc-assertion) through validator.
 * src/schema_assertion.c (5.9); its stub does nothing for a type without
 * assertions and returns SXML_STATUS_UNSUPPORTED_FEATURE otherwise.
 */
SXML_NODISCARD sxml_status sxml_schema_check_assertions(
    sxml_schema_validator *validator,
    const sxml_schema_assertion_request *request);

/* The identity constraints of one element (3.11.4). */
typedef struct sxml_schema_identity_request {
    const sxml_schema *schema;
    sxml_schema_element_declaration_index element_declaration;
    /* The element's recorded subtree, with every descendant that declares
       the keys its keyrefs refer to. */
    const sxml_schema_recorded_subtree *subtree;
} sxml_schema_identity_request;

/*
 * Evaluates the declaration's identity constraints on the element's
 * subtree and reports each violation (cvc-identity-constraint) through
 * validator. src/schema_identity.c (5.8); its stub does nothing for a
 * declaration without identity constraints and returns
 * SXML_STATUS_UNSUPPORTED_FEATURE otherwise.
 */
SXML_NODISCARD sxml_status sxml_schema_check_identity_constraints(
    sxml_schema_validator *validator,
    const sxml_schema_identity_request *request);
