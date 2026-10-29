# Project: Generating Vane's SDKs from a Schema

<!-- examples: codegen -->

Vane, the feature-flag product of the earlier projects, is one service
with three kinds of callers. The frontend SDK, `@vane/browser`, runs in
a browser page and asks the service whether each flag is on for the
user in front of it. The backend SDK, `@vane/node`, runs inside an
application on a server: it downloads every flag with its rules,
evaluates them itself and keeps them in a cache. And the console, where
the owners of a flag work, rolls flags out and pulls the kill switch.
All of them send the same facts over the network. The console sends a
rollout with a flag key, a percentage and an owner; the service reads
one. Both need the same type for it, both should refuse a percentage of
150, and the documentation for other programs should say the same
thing. When these copies are written by hand, in each SDK and in the
service, they drift apart, and the drift shows up as a bug in
production.

In this project, we'll write the facts down once, in XML, and generate
the rest. XML tools are good at this job:

- The source is declarative: types, rules and functions, without code.
- The source is typed and checked. An XSD schema describes the values,
  with facets for the rules, and another schema checks the description
  of the functions.
- The source is data. A schema is an XML document, so XQuery and XSLT
  can read it like any other document.
- The code is data too, on its way out. The generator doesn't write
  text: it builds a tree of the program, an XML document, which can be
  queried and checked before a stylesheet prints it.

We'll describe Vane's API as five *remote functions*, which a caller
calls as if they were local functions, while they run in the service.
They are the routes that the [call-chain](call-chains.md) and
[trace](traces.md) projects look at: `GET /api/getFlags` is the
function `getFlags`, and `POST /api/killFlag` is `killFlag`. From that
description, XQuery builds the tree of a TypeScript program: the types,
with runtime checks made from the facets, one client module for each
caller, which the SDKs and the console are built on, and the service's
request handler, which checks every request before the service sees
it. An XSLT stylesheet prints the tree as TypeScript, and another
writes the same API as an OpenAPI document, for other tools.

The idea comes from web frameworks that let you declare a function once
and call it from the browser, with the framework doing the network part:
SvelteKit's remote functions are one example. There, a *query* reads
data and a *command* changes it; a query can be repeated and cached, so
it goes over the network as a GET request, and a command as a POST
request. We'll build the same idea from plain parts, so you can see
every piece and change it.

The files are in `docs/book/examples/codegen`.

## The values

The values that go in and out of the functions are XSD types. Simple
types with facets say what a valid flag key, owner, state or percentage
is, and complex types put them together into requests and responses:

`vane-api.xsd`:

```xml
{{#include ../examples/codegen/vane-api.xsd}}
```

Values travel as JSON, so each complex type becomes a JSON object with
one property for each element of its sequence. An element with
`minOccurs="0"` is an optional property, such as the `retryAfter` of a
paused flag's status, and one with `maxOccurs="unbounded"` is an array,
such as the `rules` of a flag. `FlagConfig` is what an SDK needs to
evaluate a flag: its key, state, percentage and targeting rules, but
not its owners, which only the service and the console need.

## The functions

The functions are in a small language of our own, in a namespace of
our own. Each function is a `query` or a `command`, with a name, the
type of its input and the type of its output, the clients that offer
it, and a sentence that says what it does:

`api.xml`:

```xml
{{#include ../examples/codegen/api.xml}}
```

The `clients` attribute says who calls what. `getFlags` belongs to the
backend SDK, `evaluateFlags` to the frontend SDK, `getStatus` to both,
and the two commands to the console. Together the queries are the
*evaluation API* and the commands the *admin API*. The generator makes
one client module for each name, so a browser page loads only the
functions of the frontend SDK, and none of the console's.

## Checking the description

A description that the generator reads should be checked before it
does, like any other input. The description language has a schema too:

`api.xsd`:

```xml
{{#include ../examples/codegen/api.xsd}}
```

It says that names are identifiers, so they can become TypeScript names
and URL paths as they are, that no two functions have the same name,
and that every function has at least one client: `clients` is a list
of identifiers with a `minLength` of 1, which for a list counts its
items.
The description is valid, so `sxml validate` prints nothing:

```console
$ sxml validate --schema=api.xsd api.xml
```

A copy with three mistakes, a name used twice, a type name with a
space in it and a function without clients, is refused before any code
is generated:

```console
$ sed 's/name="evaluateFlags"/name="getFlags"/; s/output="KillResult"/output="Kill result"/; /rollOutFlag/s/clients="console"/clients=""/' api.xml > twice.xml
$ sxml validate --schema=api.xsd twice.xml
twice.xml:12:74: cvc-minLength-valid: attribute 'clients' has the value '', which is not valid for type 'clients': the value is shorter than the minLength facet (XSD 1.1 Part 2, 4.3.2.4); the minLength facet is '1'
twice.xml:15:48: cvc-pattern-valid: attribute 'output' has the value 'Kill result', which is not valid for type 'identifier': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[A-Za-z][A-Za-z0-9]*'
twice.xml:6:3: cvc-identity-constraint.4.1: element 'Q{https://example.org/api}query': its key sequence is that of an earlier member (unique 'Q{https://example.org/api}function-name')
```

One rule is left that a schema for `api.xml` can't check: that the type
names refer to complex types of `vane-api.xsd`, which is another
document. The generator checks that one itself, as we'll see.
[Rules Across Documents](schematron-documents.md) writes it as a
Schematron rule, which reports every wrong name at its place before
the generator runs.

## The plan

The project has four parts:

- [Building a Tree of the Program with XQuery](codegen-typescript.md):
  a library module turns the schema into types and checks, and the
  description into a client and a server, all as elements of a small
  vocabulary for TypeScript.
- [Printing the Tree with XSLT](codegen-printer.md): one template rule
  for each kind of element, and the layout in one place.
- [Running the Generated Code](codegen-running.md): a small Vane
  service, called by the clients of the two SDKs and the console, with
  Node.js.
- [A Pipeline, a Second Target and a Changing Schema](codegen-pipeline.md):
  one script that makes every file, a check that parses the generated
  code and compares its parts, an OpenAPI document made with XSLT, and
  what happens when the schema changes.
