# Introduction

Welcome to *The sxml Book*, an introduction to sxml and to the XML
languages it implements.

sxml is an XML toolchain in portable C. It parses XML and HTML, validates
documents against XSD 1.1 schemas and DTDs, and runs XPath 4.0, XQuery
4.0 and XSLT 4.0. All of its languages share one data model and one
engine. You can use it in three ways:

- as the `sxml` command, from a terminal or a shell script;
- as a C library, linked into your own program;
- as a JavaScript package, in Node.js and in browsers, through
  WebAssembly.

## Who this book is for

This book is for programmers who want to read, query, transform or check
XML (or JSON, CSV and HTML) and who want to do it with standard
languages instead of ad hoc code. It assumes you can use a terminal and
read a little code. It does not assume you know XPath, XQuery or XSLT:
each chapter introduces what it needs.

If you already know these languages from another processor, the book
still has something for you. Skim the language chapters for what is new
in the 4.0 versions, and read the projects, the chapters on embedding
sxml in C and JavaScript, on files and resolvers, and on performance.

## The running example: Vane

Most examples in this book belong to one small, made-up system called
*Vane*. Vane is a feature-flag service. An application asks Vane's SDK
whether a feature is on for a user; the SDK asks the evaluation
service, `flag-service`, which keeps the flags, their targeting rules
and their rollout percentages in a registry (`flag-registry`, backed by
PostgreSQL) and a cache (`flag-cache`, on Redis). The owners of a flag
roll it out step by step to a growing percentage of users. Before each
step the service checks the flag's guardrail metrics; after three
failed checks in a row it pauses the flag for five minutes, and a kill
switch turns a flag off at once. An audit service records the changes.

Around this system the book builds five projects, each a tool you
could use on a real project of your own:

- **The specification.** Vane's protocol is written down as an
  Internet-Draft in xml2rfc's XML vocabulary, the format IETF authors
  use, in two versions, -00 and -01. The chapters on XPath, XQuery and
  XSLT learn their languages on it, and end with tools: an index of the
  requirements, targeted edits that change only the nodes they name, a
  check, a list of what changed between versions, and the published
  draft as HTML and as RFC-style text.
- **Diagrams in SVG.** A small diagramming tool: a graph described in
  XML, a layout computed by XQuery, the drawing written as SVG by XQuery
  and by XSLT, and stylesheets that change any SVG file afterwards.
- **A self-describing state machine.** A flag's rollout lifecycle as an
  SCXML machine whose events and data carry XSD types and whose states
  carry XHTML documentation, with checks that the schemas cannot make,
  generated documentation, and a drawing.
- **A diff of call chains.** The flag service's TypeScript code before
  and after a change, read as XML through sxml's source-code parser:
  its call graph, the chains of calls from each request, and what the
  change does to them.
- **A diff of traces.** OpenTelemetry traces the services recorded
  before and after the same change, and the diff of who calls whom and
  how often, as text, JSON, HTML and SVG.

The projects feed each other in small, real ways. Each state of the
machine names, in an `implements` attribute, the requirements of the
draft it implements, and the machine's numbers (three failed checks,
ten-minute steps, a five-minute pause) are the draft's. The change
whose call chains the fourth project compares is the change the traces
of the fifth record: the code predicts one call to the registry
becoming one plus one per flag, and the traces show how often it
happens. And the diagram tool of the second project draws the services,
the state machine, the call-chain diff and the trace diff, with one
vocabulary and one module.

You don't need to read the projects in order: each one says where its
files come from, and can be read on its own once you know the languages
it uses. The book ends with one more project, outside the Vane story,
that generates code from a schema.

## How to use this book

The chapters build on each other. The first chapter installs sxml and
runs a first expression. The next three teach the three languages in the
order they build on each other: XPath, then XQuery, which contains
XPath, then XSLT, which uses XPath inside its templates. After them come
the SVG project, validation with XSD and the state-machine project,
Invisible XML for text formats, JSON, CSV and HTML, source code as XML
and the two diff projects, and then chapters on text, files, using sxml
from C and JavaScript, and performance. The appendices are for
reference.

Code examples come in two kinds. Files you create or read look like
this:

`draft.xml` (a part):

```xml
<section anchor="pause">
  <name>Pause</name>
  <t anchor="req-7">After three failed checks, a service <bcp14>MUST</bcp14> pause the flag.</t>
</section>
```

Commands you type, with the output they print, look like this. You type
the text after the `$`; the lines without a `$` are the output:

```console
$ sxml xpath '1 + 1'
2
```

All the example files are in the book's source, in the
`docs/book/examples/` directory of the sxml repository. Each project has
one directory, the way a real project has one repository: `spec` for the
specification, `svg` for the diagrams, `scxml` for the state machine,
`call-chains` and `traces` for the two diffs. The chapters that are not
part of a project have a directory of their own (`getting-started`,
`ixml`, `source-code`, `c`, `javascript`, `codegen`). Every chapter says
which directory it uses. Run its commands from inside that directory, in
order, and you will see the same output as the book: some commands write
files that later ones read. When one project uses another's file, such
as the draft in the state-machine project, its directory has a link to
the file.

## Source code

The sxml repository is at <https://github.com/dhamidi/sxml>. The book's
text is in `docs/book/` there. If you find a mistake, an example that
does not do what the book says, or a passage that confused you, please
open an issue.
