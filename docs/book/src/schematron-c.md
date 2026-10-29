# Schematron in C

<!-- examples: c -->

A program that receives documents, such as a service that stores the
drafts its users upload, can check them with Schematron itself: compile
the schema once, when it starts, and validate each document as it
arrives. This chapter does that in C, in the `c` directory of
[Embedding sxml in C](c.md), which this chapter assumes you have read:
it builds the same way and shares its `output.h`. `review.sch` there is
a link to the schema of [Patterns, Rules and
Assertions](schematron-rules.md), and `vane.xml` to the draft.

## The program

`schematron.c`:

```c
{{#include ../examples/c/schematron.c}}
```

The steps are those of every sxml program: a processor, a compiled
object, a document, a run.

1. `sxml_schematron_create` compiles the schema. Its configuration
   gives the schema's text, with its URI, which names the schema in
   errors and against which its `include` and `extends href` elements
   resolve, and a *report sink* for the errors of the schema: every
   one of them goes to the sink, and compiling goes on, as the `sxml`
   command reports them all. With the null sink, compiling stops at the
   first, and the error record has it. The configuration can also name
   a phase (`.phase_id`) and give schema parameters (`.parameters`).
2. `sxml_processor_parse_document` parses the draft. Its nodes keep
   their line and column numbers, so the findings can point at them;
   the processor's `source_location_policy` decides when documents keep
   them, and by default they do when a program compiled with the
   processor, such as the schema, can ask for them. `source_name` names
   the draft in those locations.
3. `sxml_schematron_validate_node` runs the rules over the document,
   and sends each finding to the report sink of the request as it is
   made. The outcome counts the findings and the error findings: the
   document is valid when there are none of the second.

A finding arrives as an `sxml_schematron_report`. It has every field
of the JSON report and a few more: the id, role, flag and severity, the
message, the diagnostics and properties, `is_error`, the place of the
assertion in its schema, and the subject node, with its location and
its path. The node is an item of the processor, so the sink could
evaluate an expression on it, or keep it with `sxml_item_retain`. The
texts are valid only during the call: copy what you keep.

## Running it

Build it as the other examples:

```console
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSXML_SOURCE="$SXML" > /dev/null
$ cmake --build build > /dev/null
$ ./build/schematron; echo "exit status $?"
116:7 note open-issue: Open issue issue-1 of AM: Should an owner be able to change the duration?
123:7 error anchored: A requirement needs an anchor: the SHOULD in "Security Considerations" has none.
    The next free anchor is req-12.
125:7 note open-issue: Open issue issue-2 of AB: This needs a requirement anchor of its own.
3 findings, 1 of them errors
exit status 1
```

The program prints what the `sxml` command leaves out of its text
output: the diagnostic of `anchored`, under its finding. What a program
does with a finding is its own choice: print it, return it to the user
who uploaded the draft, or refuse the draft when `error_finding_count`
is not 0.

A schema with an error never gets to the draft. Without the `ns`
element for `xs`, the diagnostic's `xs:integer` has an undeclared
prefix, and the sink receives the error, with its place; then the
create call fails with the first error in its record:

```console
$ sed 's/prefix="xs"/prefix="x"/' review.sch > bad-review.sch
$ ./build/schematron bad-review.sch; echo "exit status $?"
bad-review.sch:51:68: XPST0081: the namespace prefix is not declared
SXML_STATUS_STATIC_ERROR err:XPST0081 at 51:68: the namespace prefix is not declared
exit status 1
```

## What the program decides

`sxml_schematron_create` reads the schema from the text it is given;
the documents its `include` and `extends href` elements name, and those
that `doc` reads in its rules, come from the processor's resource
resolver, as every resource does ([Files and the Outside
World](files.md)). The program above has the default resolver, which
reads none, which is fine for `review.sch`: it reads nothing but the
draft it is given. A program that runs `flag.sch` would serve
`vane.xml` and `vane-data.xsd` with a table resolver, as `resolver.c` in
[Embedding sxml in C](c.md) serves the draft.

A compiled schema is used by the thread of its processor, like
everything else in sxml, and can validate any number of documents, one
after another. Compiling is the expensive part, which is why it happens
once: sxml turns the schema into a stylesheet's syntax tree directly,
without writing XSLT text, and compiles that.

The JavaScript package has the same two steps:
`processor.compileSchematron(text, { phase, parameters })` compiles a
schema, and its `validate(document)` returns the findings as objects,
with the same fields. [sxml in JavaScript](javascript.md) shows how to
load the package.
