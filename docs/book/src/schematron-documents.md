# Rules Across Documents

<!-- examples: codegen -->

The rules so far looked into one document, and twice into a second one:
the draft and the schema of the machine's data, read with `doc`. This
chapter makes a rule between two documents its main subject, and runs
XSD and Schematron together. It works in the `codegen` directory of
[Project: Generating Vane's SDKs from a Schema](codegen.md), which
generates Vane's SDK clients from a description of its API. You need
only two of its files here:

- `api.xml` lists Vane's remote functions, each a `query` or a
  `command`, with the names of its input and output types and the
  clients that offer it (`browser`, `node`, `console`);
- `vane-api.xsd` declares those types, as XSD complex types.

`api.xsd`, the schema of `api.xml`, checks that the names are
identifiers, that no two functions share a name and that each has a
client. One rule is left that it cannot check, because it is about
another document: each type that `api.xml` names must be a complex type
of `vane-api.xsd`. The generator checks it while it runs, and stops at
the first type it cannot find:

```console
$ sed -e 's/input="FlagsRequest"/input="Flags"/' -e 's/output="KillResult"/output="KillResults"/' -e '/killFlag/s/clients="console"/clients="console browser"/' api.xml > mistakes.xml
$ sxml xquery --param=api-file=mistakes.xml --output=mistakes-program.xml program.xq
typescript.xqm:248:10: ts:unknown-type: The function getFlags uses the type Flags, which the schema doesn't declare
```

`mistakes.xml` has two wrong type names, and one more mistake that the
next sections find. A rule finds all of them before the generator
runs.

## A rule between two documents

`api.sch`:

```xml
{{#include ../examples/codegen/api.sch}}
```

`api.xml` names its schema of values in its `types` attribute, so the
schema's variables read that document, rather than a fixed file:
`resolve-uri` resolves the name against the base URI of the description
(`base-uri(/)`), so `types="vane-api.xsd"` means the file next to
`api.xml`, wherever it is. `$complex-types` is the list of names that
the types may have, and `$used-types` the names the functions use. All
three are evaluated once per description. The prefix `xs` needs no `ns`
element: it is one of the prefixes every Schematron query knows.

The rule of the pattern `types` visits the two attributes of each
function that name types. The items are attribute nodes, so each
finding is reported at its attribute, with its column, and `name` in
the message says which of the two it is. The last two findings come
from the next pattern, in the other document:

```console
$ sxml validate --schematron=api.sch api.xml; echo "exit status $?"
exit status 0
$ sxml validate --schematron=api.sch mistakes.xml; echo "exit status $?"
mistakes.xml:3:26: type-exists: The input type Flags of getFlags is no complex type of vane-api.xsd. (api.sch:17:7)
mistakes.xml:15:48: type-exists: The output type KillResults of killFlag is no complex type of vane-api.xsd. (api.sch:17:7)
vane-api.xsd:73:3: type-used: The complex type FlagsRequest is used by no function and by no other type. (api.sch:27:7)
vane-api.xsd:143:3: type-used: The complex type KillResult is used by no function and by no other type. (api.sch:27:7)
exit status 1
```

Both wrong names are reported at once, at their places, before any code
is generated. The generator keeps its own check: a program should not
trust its input because some other tool ran first. The rule is for the
person who edits `api.xml`, and runs in the editor or the build.

## Rules over the other document

The pattern `used` turns the question around: is every complex type of
`vane-api.xsd` used, by a function or by another type? Its `documents`
attribute names the documents its rules run over instead of the
description: it is evaluated at the description's root, and a relative
name is resolved against the description's base URI, so here it is the
file that `types` names. The rule's context, `xs:schema/xs:complexType`,
is matched in that document, and `//xs:element` in its test is that
document's too, while `$used-types`, a variable of the schema, was
evaluated at the description.

A finding of such a pattern is in the other document, and says so: the
two wrong names in `mistakes.xml` left `FlagsRequest` and `KillResult`
unused, and their findings name `vane-api.xsd`, with the lines and
columns of the types there. The JSON report names the document of
every finding in its `document` field:

```console
$ sxml validate --schematron=api.sch --format=json mistakes.xml > mistakes.json
$ sxml xpath 'json-doc("mistakes.json")?files?*?findings?* ! `{?document}:{?line}: {?id}`'
"mistakes.xml:3: type-exists"
"mistakes.xml:15: type-exists"
"vane-api.xsd:73: type-used"
"vane-api.xsd:143: type-used"
```

SVRL says it as the standard asks: each `svrl:fired-rule` of the
pattern carries the URI of its document in its `document` attribute,
and the findings follow the rule that fired.

## XSD and Schematron together

The last pattern of `api.sch` reports a command that the frontend SDK
would offer: commands change flags, and a browser page must not call
them. `mistakes.xml` gives `killFlag` the clients `console browser`.
And yet `api.sch` alone did not report it.

The reason is types. Read from the file, `@clients` is one untyped
string, `"console browser"`, which is not equal to `"browser"`. In
`api.xsd`, `clients` is a list of identifiers. `--schema` together with
`--schematron` validates each document against the XSD schema first,
and then runs the rules over the validated copy, whose values have the
types the schema gives them. There, `data(@clients)` is a sequence of
two names, and `=` compares each of them:

```console
$ sxml validate --schema=api.xsd --schematron=api.sch mistakes.xml; echo "exit status $?"
mistakes.xml:/Q{https://example.org/api}api[1]/Q{https://example.org/api}query[1]/@input: type-exists: The input type Flags of getFlags is no complex type of vane-api.xsd. (api.sch:17:7)
mistakes.xml:/Q{https://example.org/api}api[1]/Q{https://example.org/api}command[2]/@output: type-exists: The output type KillResults of killFlag is no complex type of vane-api.xsd. (api.sch:17:7)
vane-api.xsd:73:3: type-used: The complex type FlagsRequest is used by no function and by no other type. (api.sch:27:7)
vane-api.xsd:143:3: type-used: The complex type KillResult is used by no function and by no other type. (api.sch:27:7)
mistakes.xml:/Q{https://example.org/api}api[1]/Q{https://example.org/api}command[2]: browser-command: The command killFlag changes flags, and a browser page must not call it. (api.sch:35:7)
exit status 1
```

The validated copy keeps no line numbers, so its findings name their
nodes by path, as `fn:path` writes it; `vane-api.xsd` is read as a file,
and keeps its lines. A rule written for typed data
should say so: run it with its XSD schema, as here, or write the test so
that it works on text too (`tokenize(@clients) = 'browser'`).

A document that the XSD schema finds invalid has no validated copy.
Its rules still run, over the document as it was read, with line
numbers, so one command reports every mistake. The copy of `api.xml`
that [Project: Generating Vane's SDKs from a Schema](codegen.md) broke
in three ways gets three XSD errors and two findings: the type name
with a space in it, and the type that this left unused:

```console
$ sed 's/name="evaluateFlags"/name="getFlags"/; s/output="KillResult"/output="Kill result"/; /rollOutFlag/s/clients="console"/clients=""/' api.xml > twice.xml
$ sxml validate --schema=api.xsd --schematron=api.sch twice.xml
twice.xml:12:74: cvc-minLength-valid: attribute 'clients' has the value '', which is not valid for type 'clients': the value is shorter than the minLength facet (XSD 1.1 Part 2, 4.3.2.4); the minLength facet is '1'
twice.xml:15:48: cvc-pattern-valid: attribute 'output' has the value 'Kill result', which is not valid for type 'identifier': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[A-Za-z][A-Za-z0-9]*'
twice.xml:6:3: cvc-identity-constraint.4.1: element 'Q{https://example.org/api}query': its key sequence is that of an earlier member (unique 'Q{https://example.org/api}function-name')
twice.xml:15:48: type-exists: The output type Kill result of killFlag is no complex type of vane-api.xsd. (api.sch:17:7)
vane-api.xsd:143:3: type-used: The complex type KillResult is used by no function and by no other type. (api.sch:27:7)
```

The XSD errors come first, then the findings. The exit status is 1 if
either finds an error.

## Where cross-document rules go

A rule between two documents could be written on either side. Put it
where the reference is: `api.xml` names the types, so `api.sch` checks
them, as `flag.sch` checks the requirements that the machine names. The
document that is referred to does not know who refers to it, and should
not have to.

Each `doc` call reads a document once per validation, however many
rules use it, and the variable that holds it is evaluated once.
`sxml validate` reads the documents that `doc` names as local files; in a program, the processor's
resource resolver decides what a schema may read, as for any query
([Files and the Outside World](files.md)).
