# Abstract Patterns, Includes and Libraries

<!-- examples: scxml -->

Three rules of `flag.sch` have one shape: an attribute holds a list of
names, and each name must be one of a set of known names. A transition's
events must be declared, its targets must be states, and a
`vane:implements` attribute must name requirements of the draft. And
rules written for one machine are worth sharing with the next: Vane
will have more machines than the flag's lifecycle. This chapter writes
the shape once, as an *abstract pattern*, puts it in a *library* that
other schemas can use, and splits a schema into files. It works in the
`scxml` directory.

## A library of rules

`vane-rules.sch`:

```xml
{{#include ../examples/scxml/vane-rules.sch}}
```

A `library` holds parts of schemas, but is not a schema: it is never
compiled on its own, only taken into one. This one has two parts.

**An abstract pattern.** A pattern with `abstract="true"` is a template:
it is not run as it is, but each pattern that names it with `is-a`
makes a copy of it, with values for its parameters. The `param`
elements declare the parameters, and `$context`, `$names`, `$known` and
`$what` in its expressions are *placeholders*: in each copy, they are
replaced by the text of the values, before the expressions are
compiled. A placeholder can stand for any part of an expression, even
for the rule's context, which a variable cannot.

**An abstract rule.** A rule with `abstract="true"`, in `rules`, is a
piece of a rule: its variables and assertions. A rule takes them with
`extends rule="..."`, as if they were written in it. `has-documentation`
asserts that the element has an XHTML paragraph somewhere inside, as the
machine's states and events have since [Documenting the Machine in
XHTML](scxml-xhtml.md).

## A schema made of parts

`names.sch` uses the library, and includes one more file:

`names.sch`:

```xml
{{#include ../examples/scxml/names.sch}}
```

`reachable.sch`:

```xml
{{#include ../examples/scxml/reachable.sch}}
```

- `extends href="vane-rules.sch"` at the top of the schema takes the
  library's children into the schema, as if they were written there.
- `include href="reachable.sch"` puts the element of another file in
  its own place: here a whole pattern, the reachable states of
  `flag.sch`. The included file has no namespace declarations of its
  own (`ns` belongs to the schema), so its expressions use the prefixes
  of the schema that includes it.
- The patterns `events`, `targets` and `requirements` are *instances* of
  the abstract pattern `names`. Each gives the four parameters, and its
  `id` names it in reports.
- `documented` gives one rule, for states and events, the assertion of
  `has-documentation`.

```console
$ sxml validate --schematron=names.sch flag.scxml
flag.scxml:92:3: reachable: No transition leads to the state archived. (reachable.sch:12:5)
flag.scxml:75:5: known-name: flag.evaluated names no declared event. (vane-rules.sch:15:7)
```

Each finding names the file and the line of the assertion that made it:
the included pattern in `reachable.sch`, the abstract pattern in the
library. The `id` of the instance, `events`, is in the JSON and SVRL
reports, as the finding's pattern.

A careless copy of the fixed machine, with an event that lost its
documentation and a transition to a state whose name is misspelled,
gets a finding from each kind of part:

```console
$ sed -e 's|<p xmlns="http://www.w3.org/1999/xhtml">An owner pulls the kill switch.</p>||' -e 's/target="paused"/target="pause"/' flag-fixed.scxml > careless.scxml
$ sxml validate --schematron=names.sch careless.scxml
careless.scxml:77:3: reachable: No transition leads to the state paused. (reachable.sch:12:5)
careless.scxml:60:5: known-name: pause names no state. (vane-rules.sch:15:7)
careless.scxml:13:5: documentation warning: The vane:event kill.request has no documentation. (vane-rules.sch:23:7)
```

## Placeholders are text

Because a placeholder is replaced by text, the value of a parameter is
a piece of an expression, not a value. Two things follow, and both are
visible in `names.sch` and the library:

- A string must be written as a string literal, with its quotes:
  `value="'declared event'"`. Without them, `declared event` would be
  read as XPath, and would not compile.
- The text is compiled where the placeholder stands, and evaluated
  there. `$known`, such as `//vane:event/@name`, is a path, and in the
  assertion the context item is the visited name, a string, where a
  path cannot start. So the abstract rule evaluates `$known` once, in a
  variable, while the context is still the element, and the assertion
  compares with the variable. Writing `test=". = $known"` would fail
  with `XPTY0004` as soon as the rule runs.

## Lists of targets

The instance `targets` checks each name of a transition's `target`,
which SCXML allows to be a list. [XSD 1.1 Features](xsd-11.md) checked
`two-targets.scxml`, whose transition enters two states, with an XSD
assertion, which could only say that it failed. The phase `graph` runs
the two patterns about states and transitions, and leaves out the
events and documentation that this small machine does not have:

```console
$ sxml validate --schematron=names.sch --phase=graph two-targets.scxml; echo "exit status $?"
exit status 0
$ sed 's/motor light/motor lights/' two-targets.scxml | sxml validate --schematron=names.sch --phase=graph -
-:11:3: reachable: No transition leads to the state light. (reachable.sch:12:5)
-:6:5: known-name: lights names no state. (vane-rules.sch:15:7)
```

The rule names the wrong target and its transition, and the other
pattern adds what follows from it: `light` can no longer be reached.

## Schema parameters

A `param` child of the schema is a *schema parameter*: a placeholder for
the whole schema, with a default. `names.sch` reads the draft through
`$draft`, `'vane.xml'`, so the same rules can check a machine against
another version of the draft. `--schematron-param` gives it another
value. Draft -00 already has every requirement the machine names:

```console
$ sxml validate --schematron=names.sch --schematron-param=draft="'vane-00.xml'" flag-fixed.scxml; echo "exit status $?"
exit status 0
```

The value is expression text, as in the schema, so the file name needs
its quotes inside the shell's. Without them, `vane-00.xml` is a path
that selects children named `vane-00.xml`, of which there are none,
`doc` of nothing is nothing, and every requirement looks unknown:

```console
$ sxml validate --schematron=names.sch --schematron-param=draft=vane-00.xml flag-fixed.scxml
flag-fixed.scxml:1:1: known-name: req-1 names no requirement of the draft. (vane-rules.sch:15:7)
flag-fixed.scxml:40:3: known-name: req-2 names no requirement of the draft. (vane-rules.sch:15:7)
flag-fixed.scxml:45:5: known-name: req-6 names no requirement of the draft. (vane-rules.sch:15:7)
flag-fixed.scxml:50:3: known-name: req-4 names no requirement of the draft. (vane-rules.sch:15:7)
flag-fixed.scxml:60:5: known-name: req-7 names no requirement of the draft. (vane-rules.sch:15:7)
flag-fixed.scxml:63:3: known-name: req-3 names no requirement of the draft. (vane-rules.sch:15:7)
flag-fixed.scxml:77:3: known-name: req-5 names no requirement of the draft. (vane-rules.sch:15:7)
flag-fixed.scxml:77:3: known-name: req-8 names no requirement of the draft. (vane-rules.sch:15:7)
```

When the same check must vary with the document, use a variable; when
it varies with the situation the schema is used in, such as which draft
or which limit, use a schema parameter; when the same shape of rule
applies to different elements, use an abstract pattern.

## Which to use

| To share | Write | Take it with |
| --- | --- | --- |
| A rule's assertions, in rules of different contexts | an abstract rule, in `rules` | `extends rule="ID"` |
| A whole pattern, with different elements and expressions | an abstract pattern, with `param` | a pattern with `is-a="ID"` and its `param` values |
| A pattern, phase or diagnostics, as it is, from another file | the element alone in a file | `include href="FILE"` |
| Many of these, as one set of rules for many schemas | a `library` | `extends href="FILE"` at the top of the schema |
