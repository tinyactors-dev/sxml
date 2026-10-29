# Checking Rules with Schematron

<!-- examples: spec -->

An XSD schema says what a document may contain: which elements, in
which order, with values of which types. Many rules of a real document
are of another kind. A requirement of the draft needs an anchor, so
that the state machine can name it. A term that the draft defines
should be used somewhere. A state of the machine should be reachable,
and every requirement it names should exist in the draft, which is
another document. These are statements about values *in relation to
each other*, across elements and across documents, and they are easier
to write as XPath tests than as types.

So far the book wrote such rules as queries: `check.xq` in
[Project: Editing a Specification Without Rewriting
It](xquery-project.md), and the machine's `check.xq` in [Checking What
the Schema Cannot](scxml-checks.md). Each query worked, and each had to
invent its own way of reporting: lines of text built by hand, an error
raised at the end so that a script stops, a `test -z` around the output
in a build. Schematron (ISO/IEC 19757-3) is the schema language for
such rules. Its rules are XPath tests, and it takes care of the rest:
every finding comes with the place in the document, a message written
for people, and a level, and `sxml validate` turns the findings into an
exit status. sxml implements its 2025 edition.

This chapter works in the `spec` directory, on the Vane draft.

## A first schema

A Schematron schema is a list of *patterns*. Each pattern has *rules*;
each rule has a *context*, an XSLT pattern that says which nodes the
rule applies to, and *assertions*, which are tests on those nodes. Here
are two rules for the draft:

`draft.sch`:

```xml
{{#include ../examples/spec/draft.sch}}
```

- The schema's elements are in the namespace
  `http://purl.oclc.org/dsdl/schematron`; `queryBinding="xslt3"` says
  that its tests are XPath and its contexts XSLT patterns.
- The first rule applies to every `t` paragraph that has a `bcp14` key
  word: a requirement. Its `assert` says what must be true, `@anchor`,
  and its text is the message when it is not.
- `value-of` puts a value of the document into the message, evaluated
  for the node the rule matched.
- The second rule applies to every term the draft defines, and asserts
  that some `xref` refers to it. Its `role="warning"` makes it a
  warning, not an error.

`sxml validate --schematron` runs the schema over the draft:

```console
$ sxml validate --schematron=draft.sch vane.xml; echo "exit status $?"
vane.xml:123:7: anchored: A requirement needs an anchor: the SHOULD in "Security Considerations" has none. (draft.sch:9:7)
exit status 1
```

Each *finding* is one line: the place in the document, the assertion's
`id`, the message, and the place of the assertion in the schema. This
is the one problem the XQuery check found in draft -01, with its line
and column. The draft is invalid, so the exit status is 1.

Break a copy of the draft as the XQuery project did, and both rules
find something:

```console
$ sed -e 's|<bcp14>MAY</bcp14> report|MAY report|' -e 's|target="term-percentage"|target="term-share"|' -e 's|<xref target="RFC6238"/>|one-time passwords|' vane.xml > broken.xml
$ sxml validate --schematron=draft.sch broken.xml
broken.xml:123:7: anchored: A requirement needs an anchor: the SHOULD in "Security Considerations" has none. (draft.sch:9:7)
broken.xml:54:11: term-used warning: The term "Percentage" is defined, and nothing refers to it. (draft.sch:17:7)
```

The warning says "warning" after its id. Alone, a warning does not make
a document invalid. Give the security requirement an anchor, and the
warning is still reported, but the exit status is 0:

```console
$ sed 's|<t>Vane messages travel|<t anchor="req-12">Vane messages travel|' broken.xml > anchored.xml
$ sxml validate --schematron=draft.sch anchored.xml; echo "exit status $?"
anchored.xml:54:11: term-used warning: The term "Percentage" is defined, and nothing refers to it. (draft.sch:17:7)
exit status 0
```

The other two mistakes of the copy are not here. A cross reference to
a term that does not exist is a rule about keys and references, which
the XSD schema already checks with `xs:keyref`:

```console
$ sxml validate --schema=vane.xsd broken.xml
broken.xml:92:9: cvc-identity-constraint.4.3: element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')
```

The key word without its markup and the reference that nothing cites
are rules `draft.sch` does not have yet; [Patterns, Rules and
Assertions](schematron-rules.md) adds them.

## Where Schematron fits

Schematron does not replace XSD, and it does not replace XQuery. Each
of the three is good at one kind of rule:

| A rule about | Example | Write it in |
| --- | --- | --- |
| Structure and types | A section has a `name` first; `docName` looks like a draft name; a percentage is from 0 to 100 | XSD |
| Unique values and references inside one document | No two anchors are the same; every `xref` names an anchor | XSD (`xs:unique`, `xs:key`, `xs:keyref`) |
| Values in relation to each other | A requirement has an anchor; a defined term is used; a command is not offered to the browser | Schematron |
| Other documents | Each requirement the machine names exists in the draft; each type the API names is in the schema of the values | Schematron |
| Whole-document properties | Every state can be reached from the initial state | Schematron, with an XPath computation |
| Changes and outputs | Fix the machine; publish the draft; generate code | XQuery or XSLT |

XSD is the first check, where a document comes in: it tells a program
what it can rely on, and gives the values their types. An XSD 1.1
assertion (`xs:assert`) can test values too, but only inside the
element it belongs to: it cannot look at ancestors, at the rest of the
document or at another document, and its messages are the processor's,
not yours. Schematron checks what remains, with messages you write, at
the level you choose; one `sxml validate` runs both, as [Rules Across
Documents](schematron-documents.md) shows. A query is still the tool
for anything that makes something: Schematron only reports.

Reach for Schematron when the rule is a sentence a reviewer would say
("this requirement has no anchor", "nothing sends this event"), when
its reader is a person who needs to know where to look, or when it
depends on another document. Reach for XSD when it is about the shape
of the document or the type of a value, and for a query when the answer
is a new document rather than a list of problems.

## What this part covers

- [Patterns, Rules and Assertions](schematron-rules.md) teaches the
  language on the draft: rules and their order, variables, messages,
  diagnostics, subjects, groups and `visit-each`.
- [Checking the State Machine](schematron-machine.md) writes the
  machine's checks as a Schematron schema, with levels and phases, and
  says what still needs XQuery.
- [Rules Across Documents](schematron-documents.md) checks the API
  description of the code generator against the schema of its values,
  and runs XSD and Schematron together.
- [Abstract Patterns, Includes and
  Libraries](schematron-reuse.md) writes a rule once and uses it for
  many names, with parameters, and shares rules between schemas.
- [Reports, Errors and Exit Statuses](schematron-reports.md) reads the
  findings as JSON and SVRL, and explains the errors of a schema and the
  query bindings.
- [Schematron in C](schematron-c.md) compiles a schema once and
  validates documents with it from a program.
