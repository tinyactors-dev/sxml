# Appendix C: Glossary

**Accumulator.** In XSLT, a value that is updated as the processor moves
through a document, by rules that match nodes. Works while streaming.

**Assertion.** In XSD 1.1, an XPath condition that a valid element
(`xs:assert`) or value (the `assertion` facet) must meet.

**Atomic value.** A value that is not a node, a function, a map or an
array: a string, a number, a boolean, a date, a duration, a QName and so
on. Each has a type.

**Attribute value template.** In XSLT and XQuery, an attribute of a
constructed element with expressions in curly braces, such as
`href="{$page}"`.

**Axis.** The direction of a step in a path: `child`, `attribute`,
`descendant`, `parent`, `ancestor`, the sibling axes and others.

**Collation.** Rules for comparing and sorting strings, named by a URI.

**Complex type.** In XSD, the type of an element with attributes or
children. Its *content model* says which children it may have.

**Constructor.** An expression that makes a new node (`<p>...</p>`,
`element { ... } { ... }`), map or array.

**Context item.** The item an expression is evaluated against: the
document given with `--context-item`, or the item a predicate is
testing. Written `.`.

**Derivation.** Making an XSD type from another: by *restriction*,
which narrows its values, or by *extension*, which adds content.

**Document node.** The root of a document's tree, above the outermost
element.

**Document order.** The order of the nodes of a document as they appear
in its text. Path expressions return nodes in document order.

**Effective boolean value.** The true or false that a condition takes
from any value: the empty sequence, the empty string, zero and NaN are
false; a node is true.

**Error code.** The name of an error, such as `err:XPTY0004`, defined by
the specifications. The same code means the same problem in every
processor.

**External variable.** A variable of a query whose value comes from
outside, such as `--param`; a parameter of the whole query.

**Facet.** A property of a simple type that restricts its values, such
as `pattern`, `maxLength` or `minInclusive`.

**FLWOR expression.** XQuery's expression of `for`, `let`, `where`,
`order by`, `group by`, `count`, window clauses and `return`.

**Identity constraint.** In XSD, a rule that values are unique
(`xs:unique`, `xs:key`) or refer to a key (`xs:keyref`).

**Item.** One member of a sequence: a node, an atomic value or a
function (maps and arrays are functions).

**Library module.** An XQuery module of declarations, with a namespace,
that queries import.

**Map, array, record.** A map associates keys with values; an array is
a list of members by position; a record type describes maps with known
keys.

**Mode.** In XSLT, a set of template rules that are used together.

**Node.** A part of a document: a document, element, attribute, text,
comment, processing instruction or namespace node.

**Optional feature.** A part of the sxml library that a program opts
into, such as HTML parsing or UCA collations.

**Output method.** How a result is written: `xml`, `html`, `xhtml`,
`text`, `json`, `csv` or `adaptive`.

**Package.** In XSLT, a named, versioned stylesheet module whose
components are private unless it makes them public.

**Pattern.** In XSLT, a condition on a node, written like a path, that
says which nodes a template rule matches.

**Pending update list.** The changes an updating XQuery expression makes,
applied all together at the end of the query.

**Predicate.** A condition in square brackets after a step or
expression, which keeps the items for which it is true.

**Processor.** In the sxml library, the object that owns the names,
types and values of everything compiled and evaluated with it.

**Resolver.** The function that a program gives the sxml library to
answer requests for resources: documents, text, modules, schemas. sxml
reads nothing else.

**Schema.** A description of what valid documents of a kind contain. In
this book, an XSD schema.

**Sequence.** The value of every expression: zero or more items in
order. Sequences don't nest.

**Sequence type.** A type of sequences, such as `xs:integer*` or
`element(book)?`.

**Serialization.** Writing a value as text or bytes, by an output
method.

**Simple type.** In XSD, a type of text: a built-in type such as
`xs:date`, or a restriction, list or union of others.

**Static and dynamic errors.** A static error is found when an
expression is compiled (a syntax error, an unknown function); a dynamic
error when it runs (division by zero, a missing document); a type error
when a value has the wrong type.

**Streaming.** Processing a document as it is parsed, without building
its tree, so memory doesn't grow with its size.

**Substitution group.** In XSD, the global elements that may appear
wherever their head element is allowed.

**Target namespace.** The namespace of the components that a schema
document declares.

**Template rule.** In XSLT, a rule with a pattern and a body that makes
the output for the nodes the pattern matches.

**Typed value.** The atomic value of a node: for a validated node, of the
type its schema gives it; otherwise untyped.

**Untyped.** The type of values from documents that were not validated,
`xs:untypedAtomic`. Converted where they are used: to numbers in
arithmetic, to strings in string comparisons.

**Validation.** Checking a document against a schema, which also gives
its nodes their types.

**Window clause.** A FLWOR clause that binds a variable to runs of
consecutive items: tumbling windows don't overlap, sliding windows may.

**XDM.** The XQuery and XPath Data Model, the model of values that
XPath, XQuery and XSLT share.
