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

Most examples in this book belong to one small, made-up product called
*Vane*. Vane is a feature-flag service: applications ask it whether a
feature is on for a user, and the owners of a flag roll the feature out
step by step to a growing percentage of users.

Vane is one service, `vane`, a TypeScript program on Node.js. Its parts
are modules: `api` takes the requests, `flags` answers them, `registry`
keeps the flags, their targeting rules and their percentages in
PostgreSQL, `cache` keeps the flags read recently in Redis, `rollout`
runs the lifecycle of each flag, and `audit` records every change in an
audit log and tells the flag's owners on their webhook. Before each
rollout step the service checks the flag's guardrail metrics; after
three failed checks in a row it pauses the flag for five minutes, and a
kill switch turns a flag off at once.

Three kinds of programs call the service, through *remote functions*,
one HTTP route per function, such as `GET /api/getFlags` and `POST
/api/killFlag`:

- The frontend SDK, `@vane/browser`, runs in a browser page and asks the
  service whether each flag is on for the user in front of it
  (`evaluateFlags`).
- The backend SDK, `@vane/node`, runs inside an application on a
  server. It downloads every flag with its rules (`getFlags`),
  evaluates the flags itself, keeps them in a cache, and keeps the
  cache current from the service's *update stream*, on which the
  service sends a line whenever a flag rolls out or is killed.
- The console, `vane-console`, is where the owners work: it rolls flags
  out (`rollOutFlag`) and pulls the kill switch (`killFlag`).

The functions the SDKs call are the *evaluation API*, the ones the
console calls the *admin API*.

Around this product the book builds six projects, each a tool you
could use on a real project of your own:

- **The specification.** Vane's protocol, the update stream between the
  service and the SDKs, is written down as an Internet-Draft in
  xml2rfc's XML vocabulary, the format IETF authors use, in two
  versions, -00 and -01. The chapters on XPath, XQuery and XSLT learn
  their languages on it, and end with tools: an index of the
  requirements, targeted edits that change only the nodes they name, a
  check, a list of what changed between versions, and the published
  draft as HTML and as RFC-style text.
- **Diagrams in SVG.** A small diagramming tool: a graph described in
  XML, a layout computed by XQuery, the drawing written as SVG by XQuery
  and by XSLT, and stylesheets that change any SVG file afterwards.
- **A self-describing state machine.** A flag's rollout lifecycle, as
  the rollout module runs it, as an SCXML machine whose events and data
  carry XSD types and whose states carry XHTML documentation, with
  checks that the schemas cannot make, generated documentation, and a
  drawing.
- **A diff of call chains.** The service's TypeScript code before and
  after a change, read as XML through sxml's source-code parser: its
  call graph, the chains of calls from each request, and what the
  change does to them.
- **A diff of traces.** OpenTelemetry traces of the same change, as the
  backend SDK, the console and the service recorded them before and
  after, and the diff of who calls whom and how often, as text, JSON,
  HTML and SVG.
- **Code from a schema.** Vane's API described once, as XSD types and
  a list of remote functions, and generated from that description: the
  client modules that the two SDKs and the console are built on, the
  service's checks of every request, and an OpenAPI document.

The projects feed each other in small, real ways. Each state of the
machine names, in an `implements` attribute, the requirements of the
draft it implements, and the machine's numbers (three failed checks,
ten-minute steps, a five-minute pause) are the draft's. The change
whose call chains the fourth project compares is the change the traces
of the fifth record: the code predicts one query to PostgreSQL becoming
one plus one per flag, and the traces show how often it happens. The
requests those two projects follow, `getFlags` and `killFlag`, are
remote functions of the sixth project's API. And the diagram tool of
the second project draws the service and its callers, the state
machine, the call-chain diff and the trace diff, with one vocabulary
and one module.

You don't need to read the projects in order: each one says where its
files come from, and can be read on its own once you know the languages
it uses.

## How to use this book

The chapters build on each other. The first chapter installs sxml and
runs a first expression. The next three teach the three languages in the
order they build on each other: XPath, then XQuery, which contains
XPath, then XSLT, which uses XPath inside its templates. After them come
the SVG project, validation with XSD and the state-machine project,
Invisible XML for text formats, JSON, CSV and HTML, source code as XML
and the two diff projects, and then chapters on text, files, using sxml
from C and JavaScript, and performance. The last project generates
Vane's SDK clients from a schema. The appendices are for reference.

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
`call-chains` and `traces` for the two diffs, and `codegen` for the
generated code. The chapters that are not part of a project have a
directory of their own (`getting-started`, `ixml`, `source-code`, `c`,
`javascript`). Every chapter says
which directory it uses. Run its commands from inside that directory, in
order, and you will see the same output as the book: some commands write
files that later ones read. When one project uses another's file, such
as the draft in the state-machine project, its directory has a link to
the file.

## Source code

The sxml repository is at <https://github.com/tinyactors-dev/sxml>. The book's
text is in `docs/book/` there. If you find a mistake, an example that
does not do what the book says, or a passage that confused you, please
open an issue.
