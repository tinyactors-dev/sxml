# Checking the State Machine

<!-- examples: scxml -->

[Checking What the Schema Cannot](scxml-checks.md) checked the flag's
state machine with a query: every state can be reached, every event the
machine handles is declared and every declared event is used, payloads
and types name something in `vane-data.xsd`, data values fit their
types, and the requirements the machine names exist in the draft. This
chapter writes the same checks as a Schematron schema, in the `scxml`
directory, and compares the two.

## The schema

`flag.sch`:

```xml
{{#include ../examples/scxml/flag.sch}}
```

It finds the four flaws that `check.xq` found:

```console
$ sxml validate --schematron=flag.sch flag.scxml; echo "exit status $?"
flag.scxml:92:3: reachable: No transition leads to the state archived. (flag.sch:34:7)
flag.scxml:31:5: used warning: Nothing handles, sends or raises the event flag.archived. (flag.sch:45:7)
flag.scxml:75:5: declared: The event flag.evaluated is not declared in vane:events. (flag.sch:41:7)
flag.scxml:2:1: implemented warning: Nothing in the machine implements the draft's req-1. (flag.sch:96:7)
exit status 1
```

and the five mistakes of the copy that [Checking What the Schema
Cannot](scxml-checks.md) made, which the XSD schemas do not see:

```console
$ sed -e 's/expr="3"/expr="30"/' \
      -e 's/vane:type="vane:seconds"/vane:type="seconds"/' \
      -e 's/vane:payload="vane:rollout"/vane:payload="vane:rolout"/' \
      -e 's/req-5 req-8/req-5 req-12/' flag.scxml > worse.scxml
$ sxml validate --schematron=flag.sch worse.scxml
worse.scxml:92:3: reachable: No transition leads to the state archived. (flag.sch:34:7)
worse.scxml:31:5: used warning: Nothing handles, sends or raises the event flag.archived. (flag.sch:45:7)
worse.scxml:75:5: declared: The event flag.evaluated is not declared in vane:events. (flag.sch:41:7)
worse.scxml:7:5: payload: The payload vane:rolout of rollout.request is no element of vane-data.xsd. (flag.sch:67:7)
worse.scxml:38:5: value: The value 30 of maxFailures is not a vane:failure-limit. (flag.sch:78:7)
worse.scxml:39:5: type: The type seconds of stepSeconds is no simple type of vane-data.xsd. (flag.sch:75:7)
worse.scxml:40:5: type: The type seconds of pauseSeconds is no simple type of vane-data.xsd. (flag.sch:75:7)
worse.scxml:80:3: known-requirement: The draft has no requirement req-12. (flag.sch:88:7)
worse.scxml:2:1: implemented warning: Nothing in the machine implements the draft's req-1. (flag.sch:96:7)
```

The fixed machine of [Checking What the Schema Cannot](scxml-checks.md)
passes:

```console
$ sxml validate --schematron=flag.sch flag-fixed.scxml; echo "exit status $?"
exit status 0
```

## Reading the schema

Two prefixes, `sc` for SCXML and `vane` for Vane's own attributes and
elements, are declared with `ns`, as the machine declares them with
`xmlns`; the names in the rules use them.

**States.** The pattern's variable `$reachable` is the computation of
`local:reachable` in `check.xq`, written as one XPath expression: a
`let`, then `fold-left` over as many passes as there are states, each
adding the targets of the states reached so far. `tokenize` splits each
`target`, which SCXML allows to be a list of states (see [XSD 1.1
Features](xsd-11.md)). A pattern's variable
is evaluated once, for the whole machine, and then each state is only
compared with it.

**Events.** A transition's `event` attribute holds a list of event
names, so the rule visits each name (`visit-each`, from [Patterns,
Rules and Assertions](schematron-rules.md)) and asserts that it is
declared. The finding points at the transition that waits for the
undeclared event, where the query could only name the event. An unused
event is a warning: it is harmless, and an owner may be about to use it.

**Data.** `$schema` reads `vane-data.xsd` as a document, as the query
did, and the payload rule compares QNames. `resolve-QName` turns the
attribute's text, `vane:rollout`, into a QName with the namespace the
prefix `vane` has *in the machine*, so the machine could use another
prefix and still pass. For types, the schema needs XSLT itself, which
the next section explains.

**Requirements.** `doc('vane.xml')` reads the draft. A relative URI in a
rule resolves against the schema's own location, so the draft is found
next to `flag.sch`, wherever the machine is. The first rule checks the
anchors the machine names; the last one goes the other way. Its context
is the machine's root, and it visits each anchor of a MUST or MUST NOT
requirement *of the draft*: the items are strings from another
document, and each finding is reported at the machine's root element,
line 2, which is what it is about.

## XSLT inside a schema

`queryBinding="xslt3"` says more than which language the tests are in:
under an XSLT binding, a schema can hold XSLT declarations, and sxml
compiles them into the stylesheet it makes of the schema. `flag.sch`
uses two:

- `xsl:import-schema` imports `vane-data.xsd`, so its simple types are
  known, and each has a constructor function. `function-lookup($type,
  1)` finds the constructor of a type by its name, as `check.xq` did;
  it is empty when the type does not exist, which is the `type` check.
- `xsl:function` declares `check:fits`, which calls the constructor
  with a value and says whether that works. A constructor raises an
  error for a value that is not one of its type, and an XPath
  expression cannot catch errors; `xsl:try` can, so the function
  returns `false()` instead. The query did the same with XQuery's
  `validate` and `try`.

A test that raised the error itself would stop the validation, so the
assertion `value` calls the function only for types that exist.

## What Schematron does better

The checks are the same, and so are their answers. What changed is
everything around them:

- **Places.** Each finding has its line and column in the machine, and
  the place of the rule in the schema. `check.xq` printed names and left
  the reader to search for them.
- **Messages.** Each assertion has its own message, written as a
  sentence, next to its test. The query built its lines in the
  expressions that computed them.
- **Levels.** An unused event and a requirement nothing implements yet
  are warnings; the rest are errors. The query had one level: every line
  was a problem.
- **Exit status.** `sxml validate` exits with 1 when there is an error,
  and 0 when there are only warnings, so a build runs it as it is. The
  query needed a shell test around its output.
- **Reports for programs.** The same findings are available as JSON and
  as SVRL, the report format of the standard ([Reports, Errors and Exit
  Statuses](schematron-reports.md)).

## Phases

Not every check fits every moment. While an owner sketches a new
machine, it names no requirements yet, and comparing it with the draft
gives nothing but warnings. A *phase* names a set of patterns to run:
`machine` runs `states`, `events` and `data`, and leaves the draft
alone. `--phase` chooses it:

```console
$ sxml validate --schematron=flag.sch --phase=machine flag.scxml
flag.scxml:92:3: reachable: No transition leads to the state archived. (flag.sch:34:7)
flag.scxml:31:5: used warning: Nothing handles, sends or raises the event flag.archived. (flag.sch:45:7)
flag.scxml:75:5: declared: The event flag.evaluated is not declared in vane:events. (flag.sch:41:7)
```

Without `--phase`, the schema's `defaultPhase` attribute chooses, and
without that, every pattern runs (`#ALL`). The 2025 edition adds a
third way: with `--phase='#ANY'`, each document chooses for itself. The
first phase, in the order of the schema, whose `when` expression is
true for the document is used; if none is, every pattern runs.
`machine`'s `when` is true for a machine that names no requirement.
Remove the `vane:implements` attributes, and the sketch gets the
machine's checks, while the full machine gets all of them:

```console
$ sed 's/ vane:implements="[^"]*"//' flag.scxml > sketch.scxml
$ sxml validate --schematron=flag.sch --phase='#ANY' sketch.scxml flag.scxml
sketch.scxml:92:3: reachable: No transition leads to the state archived. (flag.sch:34:7)
sketch.scxml:31:5: used warning: Nothing handles, sends or raises the event flag.archived. (flag.sch:45:7)
sketch.scxml:75:5: declared: The event flag.evaluated is not declared in vane:events. (flag.sch:41:7)
flag.scxml:92:3: reachable: No transition leads to the state archived. (flag.sch:34:7)
flag.scxml:31:5: used warning: Nothing handles, sends or raises the event flag.archived. (flag.sch:45:7)
flag.scxml:75:5: declared: The event flag.evaluated is not declared in vane:events. (flag.sch:41:7)
flag.scxml:2:1: implemented warning: Nothing in the machine implements the draft's req-1. (flag.sch:96:7)
```

A phase can also have variables of its own, and a `from` expression
that selects the nodes its patterns start from instead of the whole
document. A phase that the schema does not have is an error:

```console
$ sxml validate --schematron=flag.sch --phase=quick flag.scxml
flag.sch:5:1: schematron:phase: the schema has no phase of this id (5.4.13): quick
```

## What still needs XQuery

Schematron reports; it never changes a document or makes a new one. Two
parts of the state-machine project stay queries and stylesheets:

- **Fixing.** `fix.xq` makes the fixed machine with an update, and
  Schematron's findings say where to look but not what to write.
  Running `flag.sch` after the fix, as above, is how a build knows the
  fix worked.
- **Making things from the machine.** The documentation of [Documenting
  the Machine in XHTML](scxml-xhtml.md) and the drawing of [Drawing the
  Machine](scxml-drawing.md) are outputs, not findings.

A check whose answer is a list of problems belongs in Schematron. A
check that is part of a larger computation, such as the generator's
own check of the types in [The rule the schema couldn't
check](codegen-typescript.md#the-rule-the-schema-couldnt-check), may
stay in the program that needs it; [Rules Across
Documents](schematron-documents.md) writes that one as a rule too.
