# Appendix C: Glossary

**Abstract type.** In XSD, a type that no element may have itself, only
a type derived from it (`abstract="true"`). An abstract element
declaration may appear only through its substitution group.

**Accumulator.** In XSLT, a value that is updated as the processor moves
through a document, by rules that match nodes. Works while streaming.

**Admin API.** In Vane, the remote functions that the console calls to
change flags: the commands `rollOutFlag` and `killFlag`.

**Anchor.** In an xml2rfc document, the name of a section, paragraph,
term or reference, given by its `anchor` attribute; `xref` refers to
it with `target`. Anchors are names, not numbers: the renderer numbers
sections and requirements by their position.

**Assertion.** In XSD 1.1, an XPath condition that a valid element
(`xs:assert`) or value (the `assertion` facet) must meet.

**Atomic value.** A value that is not a node, a function, a map or an
array: a string, a number, a boolean, a date, a duration, a QName and so
on. Each has a type.

**Attribute value template.** In XSLT and XQuery, an attribute of a
constructed element with expressions in curly braces, such as
`href="{$page}"`.

**Audit module.** The module of the Vane service that records every
change of a flag: it writes an event to the audit log in PostgreSQL and
posts it to the owners' webhook.

**Axis.** The direction of a step in a path: `child`, `attribute`,
`descendant`, `parent`, `ancestor`, the sibling axes and others.

**Backend SDK.** In Vane, `@vane/node`, the SDK for applications on a
server: it downloads every flag with its rules (`getFlags`), evaluates
the flags itself, keeps them in a cache and keeps the cache current from
the update stream.

**Barycentre.** In a layer sweep, the mean position of a vertex's
neighbours in the next layer, by which the layer is sorted.

**BCP 14 key word.** One of the words of RFC 2119 and RFC 8174 that say
how strong a requirement is: MUST, MUST NOT, REQUIRED, SHALL, SHALL
NOT, SHOULD, SHOULD NOT, RECOMMENDED, NOT RECOMMENDED, MAY and
OPTIONAL. xml2rfc marks them with `bcp14`.

**Boundary whitespace.** In an XQuery direct constructor, whitespace
that stands alone between tags or enclosed expressions. It is dropped
unless the query declares `boundary-space preserve`.

**Call chain.** A path of calls in a program, from an *entry* down to a
function that calls no other function of the program.

**Call graph.** Which function of a program calls which: a graph with a
node for each function and an edge for each call.

**Cardinality.** In the traces project, how many times one participant
calls another in a request.

**Client module.** In the codegen project, a generated TypeScript module
for one caller of the API, such as the frontend SDK, with one async
function for each remote function that caller may call.

**Collation.** Rules for comparing and sorting strings, named by a URI.

**Complex type.** In XSD, the type of an element with attributes or
children. Its *content model* says which children it may have.

**Conditional type assignment.** In XSD 1.1, choosing an element's type
by a condition on its attributes, with `xs:alternative`.

**Constructor.** An expression that makes a new node (`<p>...</p>`,
`element { ... } { ... }`), map or array.

**Context item.** The item an expression is evaluated against: the
document given with `--context-item`, or the item a predicate is
testing. Written `.`.

**Core rules.** The rules that RFC 5234, the ABNF standard, defines for
every grammar in its Appendix B.1, such as `ALPHA`, `DIGIT`, `SP` and
`CRLF`. A grammar uses them without defining them.

**Default attributes.** In XSD 1.1, an attribute group that every
complex type of a schema document gets, named by `defaultAttributes`
on `xs:schema`.

**Default collection.** The collection that `collection()` returns
without an argument; for the sxml command, the XML files of the
directory given with `--collection`.

**Derivation.** Making an XSD type from another: by *restriction*,
which narrows its values, or by *extension*, which adds content.

**Depth-first search.** Visiting a graph by following an edge from the
node reached last, as far as it goes, and only then coming back to try
the next edge. In the SVG project, an edge that leads back to a node on
the way the search came closes a cycle.

**Document node.** The root of a document's tree, above the outermost
element.

**Document order.** The order of the nodes of a document as they appear
in its text. Path expressions return nodes in document order.

**Editorial note.** A note to the authors and reviewers of a draft, not
part of the published text: `cref` in xml2rfc.

**Effective boolean value.** The true or false that a condition takes
from any value: the empty sequence, the empty string, zero and NaN are
false; a node is true.

**Entry.** In a call graph, a function that no call of the program
reaches, where call chains start, such as the function that starts a
server.

**Error code.** The name of an error, such as `err:XPTY0004`, defined by
the specifications. The same code means the same problem in every
processor.

**Evaluation API.** In Vane, the remote functions that the SDKs call to
read flags: the queries `getFlags`, `evaluateFlags` and `getStatus`.

**Executable content.** In SCXML, the actions that run on a transition
and when a state is entered or left: `assign`, `log`, `raise`, `send`
and others.

**External variable.** A variable of a query whose value comes from
outside, such as `--param`; a parameter of the whole query.

**Facet.** A property of a simple type that restricts its values, such
as `pattern`, `maxLength` or `minInclusive`.

**Feature flag.** A switch in a running application that turns a
feature on or off for some of its users without a new release. In
Vane, the book's example product, the backend SDK evaluates the flags
itself, and the frontend SDK asks the Vane service whether each flag is
on for a user.

**FLWOR expression.** XQuery's expression of `for`, `let`, `where`,
`order by`, `group by`, `count`, window clauses and `return`.

**Frontend SDK.** In Vane, `@vane/browser`, the SDK for browser pages:
it asks the Vane service whether each flag is on for the user in front
of it (`evaluateFlags`).

**Guardrail metric.** A measurement, such as an error rate, that must
stay healthy while a feature flag is rolled out. Vane checks the
guardrail metrics before each step, and pauses a flag after three failed
checks in a row.

**Identity constraint.** In XSD, a rule that values are unique
(`xs:unique`, `xs:key`) or refer to a key (`xs:keyref`).

**Import precedence.** In XSLT, the rule that a declaration in an
importing stylesheet beats any declaration it imports, whatever their
priorities.

**Instrumentation scope.** In OpenTelemetry, the library that recorded
a span, such as the instrumentation for HTTP or for PostgreSQL. In OTLP,
a resource's spans are grouped by scope.

**Internal span.** A span of work inside one program, such as one
function of a module. It is not a call, so it has no callee.

**Internet-Draft.** A working document of the IETF, the body that
publishes the RFCs. Drafts are named like
`draft-example-vane-protocol-01`, the last part being the version, and
expire after six months.

**Item.** One member of a sequence: a node, an atomic value or a
function (maps and arrays are functions).

**Kill switch.** A way to turn a feature flag off at once for every
user. In Vane, the `killFlag` command of the admin API, which the
console sends for one of the flag's owners.

**Layer.** In the SVG project, one of the horizontal bands a layered
layout puts the vertices of a diagram in, so that every edge points
down.

**Layout.** In the SVG project, the place of each box of a diagram and
the path of each edge, computed from the graph by the layered method in
five phases: break the cycles, assign layers, order the layers, place,
route.

**Library module.** An XQuery module of declarations, with a namespace,
that queries import.

**Map, array, record.** A map associates keys with values; an array is
a list of members by position; a record type describes maps with known
keys.

**Model order.** The order in which a description lists its nodes and
edges. A layout that follows the model order where two choices are
equally good lets the author steer it.

**Mode.** In XSLT, a set of template rules that are used together.

**N+1.** A pattern of calls in which a program asks once for a list of
keys and then once more for each item, instead of once for all of them:
N + 1 calls where one would do.

**Namespace import, named import.** In TypeScript, `import * as
registry from "./registry.ts"` makes `registry` stand for a whole
module; `import { listFlags } from "./flags.ts"` makes `listFlags`
stand for one function of a module.

**Node.** A part of a document: a document, element, attribute, text,
comment, processing instruction or namespace node.

**Normal form.** In the traces project, a trace file as XML: one
`traces` element with a `service` element for each program that
recorded spans and a tree of `span` elements for each trace. The later
queries and stylesheets work on it.

**Open content.** In XSD 1.1, elements that a content model does not
name allowed among the ones it does, by a wildcard in `xs:openContent`.

**Optional feature.** A part of the sxml library that a program opts
into, such as HTML parsing or UCA collations.

**OTLP.** The OpenTelemetry Protocol, how programs send their spans to
a collector. Its JSON encoding is the format of the traces project's
files.

**Output method.** How a result is written: `xml`, `html`, `xhtml`,
`text`, `json`, `csv` or `adaptive`.

**Override.** In XSD 1.1, using a schema document with some of its
components replaced, with `xs:override`.

**Package.** In XSLT, a named, versioned stylesheet module whose
components are private unless it makes them public.

**Palette.** In the SVG project, a map from each colour of a drawing to
the one that replaces it, as a dark theme does.

**Participant.** In the traces project, a program, database or webhook that
takes part in a request, as the caller or the callee of a call.

**Pattern.** In XSLT, a condition on a node, written like a path, that
says which nodes a template rule matches.

**Pause.** In Vane, a time in which a flag refuses every rollout, after
too many failed checks.

**Payload.** The data an event carries. In the state-machine project,
an event declaration names its payload's element in `vane-data.xsd`.

**Pending update list.** The changes an updating XQuery expression makes,
applied all together at the end of the query.

**Predicate.** A condition in square brackets after a step or
expression, which keeps the items for which it is true.

**Printer.** A program that writes a syntax tree as source text, with
the keywords, punctuation and layout of the language. In [Printing the
Tree with XSLT](codegen-printer.md), an XSLT stylesheet with one template
rule for each kind of element.

**Processor.** In the sxml library, the object that owns the names,
types and values of everything compiled and evaluated with it.

**Remote function.** A function that runs on a server and that a
client, such as a browser page, calls as if it were local, over one
HTTP route. Vane's API is a set of remote functions: a *query* reads
data and goes over the network as a GET request, a *command* changes
data and goes as a POST request.

**Requirement.** In a specification, a statement that an
implementation must, should or may do something, with a BCP 14 key
word. In the book's draft, a `t` paragraph with an `anchor` and a
`bcp14` element.

**Resolver.** The function that a program gives the sxml library to
answer requests for resources: documents, text, modules, schemas. sxml
reads nothing else.

**Resource.** In OpenTelemetry, what recorded a set of spans, usually a
service, described by attributes such as `service.name` and
`service.version`.

**Result document.** In XSLT, an output besides the principal result,
made with `xsl:result-document`; the sxml command writes them below
`--result-directory`.

**Rollout.** Turning a feature flag on for a growing percentage of
users, in steps.

**Schema.** A description of what valid documents of a kind contain. In
this book, an XSD schema.

**Schema-aware.** A processor that can validate documents inside a
query or stylesheet and use the types the schema gives their nodes.
sxml is schema-aware.

**SCXML.** State Chart XML, the W3C's XML vocabulary for state
machines.

**Semantic conventions.** OpenTelemetry's agreed names and meanings of
span attributes, such as `http.request.method`, `http.route` and
`db.system.name`.

**Sequence.** The value of every expression: zero or more items in
order. Sequences don't nest.

**Sequence type.** A type of sequences, such as `xs:integer*` or
`element(book)?`.

**Serialization.** Writing a value as text or bytes, by an output
method.

**Simple type.** In XSD, a type of text: a built-in type such as
`xs:date`, or a restriction, list or union of others.

**Span, span kind.** In tracing, one operation of a program, with a
name, a start, an end and a parent. Its kind says what it is: a
*server* span handles a request, a *client* span calls something else,
an *internal* span is work inside the program.

**State machine.** A system that is in one of a set of states at a time
and moves between them on events, by transitions.

**Static and dynamic errors.** A static error is found when an
expression is compiled (a syntax error, an unknown function); a dynamic
error when it runs (division by zero, a missing document); a type error
when a value has the wrong type.

**Streaming.** Processing a document as it is parsed, without building
its tree, so memory doesn't grow with its size.

**String constructor.** In XQuery, a string written between
``` ``[ ``` and ``` ]`` ```, with expressions between `` `{ `` and
`` }` ``; everything else, braces and quotes included, is literal text.
The simplest way to write a line or two of code; for whole files, see
*Syntax tree*.

**Substitution group.** In XSD, the global elements that may appear
wherever their head element is allowed.

**SVG.** Scalable Vector Graphics, the W3C's XML vocabulary for
drawings, in the namespace `http://www.w3.org/2000/svg`.

**Syntax tree.** A program as a tree of its parts. A *concrete* syntax
tree, as `tree-sitter:parse` returns, holds every token and space, so its
string value is the source; an *abstract* one holds only the parts that
vary, such as names, types and statements, and a printer adds the rest.

**Target namespace.** The namespace of the components that a schema
document declares.

**Template rule.** In XSLT, a rule with a pattern and a body that makes
the output for the nodes the pattern matches.

**Trace.** All the spans of one request through a system,
which share a trace id: a tree of spans.

**Typed value.** The atomic value of a node: for a validated node, of the
type its schema gives it; otherwise untyped.

**Untyped.** The type of values from documents that were not validated,
`xs:untypedAtomic`. Converted where they are used: to numbers in
arithmetic, to strings in string comparisons.

**Update stream.** In Vane, the connection an SDK holds open to the
Vane service, on which the service sends a line whenever a flag rolls
out or is killed, and an SDK asks for the status of a flag. The Vane
protocol defines its messages.

**Validation.** Checking a document against a schema, which also gives
its nodes their types.

**Vane.** The made-up feature-flag product of the book's projects: one
service, `vane`, a TypeScript program on Node.js made of modules (api,
flags, registry, cache, rollout, audit) over PostgreSQL and Redis. It
offers an evaluation API to its two SDKs, `@vane/browser` and
`@vane/node`, an admin API to its console, and an update stream.

**viewBox.** The attribute of an `svg` element that gives the area of
its coordinates that is drawn (left, top, width and height). Changing
`width` and `height` but not `viewBox` scales the drawing.

**Wildcard.** In XSD, `xs:any` or `xs:anyAttribute`: a place in a
content model that allows elements or attributes by namespace instead
of by name. `processContents` says whether they are validated.

**Window clause.** A FLWOR clause that binds a variable to runs of
consecutive items: tumbling windows don't overlap, sliding windows may.

**Worklist.** The list of unfinished items in a loop that replaces a
recursive function: the loop takes one off, and puts back what it finds
still to do, until the list is empty.

**XDM.** The XQuery and XPath Data Model, the model of values that
XPath, XQuery and XSLT share.

**xml2rfc.** The IETF's XML vocabulary for Internet-Drafts and RFCs
(version 3, RFC 7991), and the tool that renders it as text and HTML.
