# Project: Generating Code from a Schema

<!-- examples: codegen -->

A web application that talks to its own server has the same facts in
many places. The browser sends a loan request with a book and a member;
the server reads one. Both need the same type for it, both should
refuse a member id that isn't one, and the documentation for other
programs should say the same thing. When these copies are written by
hand, they drift apart, and the drift shows up as a bug in production.

In this project, we'll write the facts down once, in XML, and generate
the rest. XML tools are good at this job:

- The source is declarative: types, rules and functions, without code.
- The source is typed and checked. An XSD schema describes the values,
  with facets for the rules, and another schema checks the description
  of the functions.
- The source is data. A schema is an XML document, so XQuery and XSLT
  can read it like any other document and turn it into text.

We'll describe a small API for the library: three *remote functions*,
which a browser page calls as if they were local functions, while they
run on the server. From that description, XQuery generates TypeScript:
the types, with runtime checks made from the facets, a browser client,
and the server's request handler. XSLT then writes the same API as an
OpenAPI document, for other tools.

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
types with facets say what a valid id, genre or number of days is, and
complex types put them together into requests and responses:

`library.xsd`:

```xml
{{#include ../examples/codegen/library.xsd}}
```

Values travel as JSON, so each complex type becomes a JSON object with
one property for each element of its sequence. An element with
`minOccurs="0"` is an optional property, and one with
`maxOccurs="unbounded"` is an array.

## The functions

The functions are in a small language of our own, in a namespace of
our own. Each function is a `query` or a `command`, with a name, the
type of its input and the type of its output, and a sentence that says
what it does:

`api.xml`:

```xml
{{#include ../examples/codegen/api.xml}}
```

## Checking the description

A description that the generator reads should be checked before it
does, like any other input. The description language has a schema too:

`api.xsd`:

```xml
{{#include ../examples/codegen/api.xsd}}
```

It says that names are identifiers, so they can become TypeScript names
and URL paths as they are, and that no two functions have the same name.
The description is valid, so `sxml validate` prints nothing:

```console
$ sxml validate --schema=api.xsd api.xml
```

A copy with two mistakes, a name used twice and a type name with a
space in it, is refused before any code is generated:

```console
$ sed 's/name="searchBooks"/name="getBook"/; s/output="Loan"/output="Loan record"/' api.xml > twice.xml
$ sxml validate --schema=api.xsd twice.xml
twice.xml:9:50: cvc-pattern-valid: attribute 'output' has the value 'Loan record', which is not valid for type 'identifier': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[A-Za-z][A-Za-z0-9]*'
twice.xml:6:3: cvc-identity-constraint.4.1: element 'Q{https://example.org/api}query': its key sequence is that of an earlier member (unique 'Q{https://example.org/api}function-name')
```

One rule is left that a schema for `api.xml` can't check: that the type
names refer to complex types of `library.xsd`, which is another
document. The generator checks that one itself, as we'll see.

## The plan

The project has three parts:

- [Generating TypeScript with XQuery](codegen-typescript.md): a library
  module turns the schema into types and checks, and the description
  into a client and a server.
- [Running the Generated Code](codegen-running.md): a small server and
  a client that calls it, with Node.js.
- [A Pipeline, a Second Target and a Changing Schema](codegen-pipeline.md):
  one script that makes every file, an OpenAPI document made with XSLT,
  and what happens when the schema changes.
