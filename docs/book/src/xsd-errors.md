# Reading Validation Errors

<!-- examples: spec -->

Validation errors are meant to be read by people, and acted on. This
section explains what sxml's messages contain, goes through the ones
you'll meet most in drafts with their usual fixes, and shows how
programs read the same report as JSON.

## The parts of a message

Every message has the same parts:

```text
bad-keyword.xml:12:35: cvc-enumeration-valid: element 'bcp14' has the value 'MIGHT', ...
```

- the file, line and column of the element or attribute;
- the *constraint*: the name that the XSD specification gives the rule
  that failed. You can look it up in XSD 1.1 Part 1 (structures,
  constraints starting `cvc-` for "validation contributions") or Part 2
  (datatypes, the facets);
- a description: which element or attribute, its value, and what was
  expected.

## Errors in the schema

A schema is compiled before any document is read, and when it has
errors, no document is checked. Schema errors have codes that don't
start with `cvc-`: `src-` for the XML representation of the schema,
`cos-` for rules about components, and others. This schema for an
editorial note has three mistakes:

`bad-schema.xsd`:

```xml
{{#include ../examples/spec/bad-schema.xsd}}
```

```console
$ sxml validate --schema=bad-schema.xsd xsd-dtd.xml; echo "exit status $?"
bad-schema.xsd:8:7: src-attribute: An attribute's value is not allowed: use
exit status 1
```

`use` allows `optional`, `required` and `prohibited`. sxml first reads
the schema documents, then resolves the references between them, and
reports the errors of one step before it goes to the next, so fix what
it reports and run it again:

```console
$ sed 's/use="always"/use="required"/' bad-schema.xsd > fixed.xsd
$ sxml validate --schema=fixed.xsd xsd-dtd.xml
fixed.xsd:6:7: src-resolve: A QName value names no component of its kind: Q{http://www.w3.org/2001/XMLSchema}NCname
fixed.xsd:7:7: src-resolve: A QName value names no component of its kind: initials
```

`src-resolve` means a name in the schema points at nothing: here a
misspelled built-in type (it's `xs:NCName`) and a type that was never
declared. The name is shown as an expanded name, with its namespace in
braces, so you can see whether a prefix went wrong too.

## Errors in documents

Three short drafts, each with the mistakes of one kind. The first has
key words that aren't key words:

`bad-keyword.xml`:

```xml
{{#include ../examples/spec/bad-keyword.xml}}
```

```console
$ sxml validate --schema=vane.xsd bad-keyword.xml
bad-keyword.xml:11:32: cvc-enumeration-valid: element 'bcp14' has the value 'must', which is not valid for type 'keyword': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'MUST', 'MUST NOT', 'REQUIRED', 'SHALL', 'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', ...
bad-keyword.xml:12:35: cvc-enumeration-valid: element 'bcp14' has the value 'MIGHT', which is not valid for type 'keyword': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'MUST', 'MUST NOT', 'REQUIRED', 'SHALL', 'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', ...
```

BCP 14's key words count only in capitals (RFC 8174), and the schema
agrees: enumerations compare exactly. When a list of facets is long,
the message shows the first eight and `...`.

The second uses an anchor twice:

`bad-anchor.xml`:

```xml
{{#include ../examples/spec/bad-anchor.xml}}
```

```console
$ sxml validate --schema=vane.xsd bad-anchor.xml
bad-anchor.xml:12:7: cvc-identity-constraint.4.1: element 't': its key sequence is that of an earlier member (unique 'anchor')
bad-anchor.xml:15:5: cvc-identity-constraint.4.1: element 'section': its key sequence is that of an earlier member (unique 'anchor')
```

The message points at the second use, and names the constraint,
`anchor`, so you can find it in the schema.

An anchor must also be a name, an `xs:NCName`, which has no spaces. An
editor who writes the anchor of the Pause section with one gets two
messages for one mistake:

```console
$ sed 's/<section anchor="pause">/<section anchor="the pause">/' vane.xml \
  | sxml validate --schema=vane.xsd -
-:106:14: cvc-datatype-valid.1.2.1: attribute 'anchor' has the value 'the pause', which is not valid for type 'anchor'
-:92:10: cvc-identity-constraint.4.3: element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')
```

The first says what is wrong with the value. The second follows from
it: a value that is not valid for its type is no key at all, so the
section has no anchor that the `xref` to `pause` in the Rollout section
could name, and the keyref reports that `xref`. Fix the first message,
and the second goes away with it.

The third draft refers to anchors that don't exist:

`bad-xref.xml`:

```xml
{{#include ../examples/spec/bad-xref.xml}}
```

```console
$ sxml validate --schema=vane.xsd bad-xref.xml
bad-xref.xml:11:27: cvc-identity-constraint.4.3: element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')
bad-xref.xml:12:41: cvc-identity-constraint.4.3: element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')
```

This short draft has no terminology section and no section `pause`;
they were copied from the full draft. The reference to `RFC2119` is
fine: the draft has that reference.

The constraints you'll see most in documents, and what they usually
mean:

| Constraint | Meaning | Usual fix |
| --- | --- | --- |
| `cvc-elt.1` | No global declaration for the document element | Check the element's namespace, and that the right schema is given |
| `cvc-complex-type.2.4` | An element is not allowed here, or the content is incomplete | Look at the expected elements it lists: order, spelling, namespace |
| `cvc-complex-type.2.1`, `2.3` | Text where the content is empty or element-only | Remove the text, or make the type mixed |
| `cvc-complex-type.3.2.2` | An attribute that the type doesn't declare | Declare it, or remove it |
| `cvc-complex-type.4` | A required attribute is missing | Add it |
| `cvc-datatype-valid.1.2.1` | The text is not a value of the type | Check the lexical form: `2026`, not `26` |
| `cvc-pattern-valid`, `cvc-enumeration-valid`, `cvc-maxInclusive-valid`, ... | A value breaks a facet; the message names it | Compare the value with the facet it shows |
| `cvc-au` | A fixed attribute has another value | Use the fixed value |
| `cvc-type.2`, `cvc-elt.2` | An element has an abstract type or declaration | Give a derived type with `xsi:type`, or use a member of the group |
| `cvc-elt.4.3` | `xsi:type` names a type not derived from the declared one | Use a derived type |
| `cvc-id.2` | An `xs:ID` value is used twice | Make the ids unique |
| `cvc-identity-constraint.4.1`, `4.2.2` | Two elements have the same key | Make the keys unique |
| `cvc-identity-constraint.4.2.1` | An element of a key has no valid value for a field | Add the value, or fix the one that is reported before it |
| `cvc-identity-constraint.4.3` | A keyref names a key that doesn't exist | Add the target, or fix the reference |
| `cvc-assertion`, `cvc-assertions-valid` | An assertion is false | Read the assertion in the message |

## After an element that is not allowed

When an element is not allowed where it is, sxml reports it and then
goes on with the rest of the content of its parent, so one run finds
the later mistakes too. It guesses what the author meant: if the
element is allowed further on in the content model, the elements
before it are taken as missing, and checking continues from there;
otherwise the element is taken as extra and skipped. Either way the
element's own content is still checked against its declaration.

`bad-content.xml`, from [Complex Types and Content
Models](xsd-complex-types.md), had the author before the title. The
author is allowed after a title, so sxml takes the title as missing
and goes on after the author; then the real title comes too late, and
is reported as well:

```console
$ sxml validate --schema=vane.xsd bad-content.xml 2>&1 | head -3
bad-content.xml:1:6: cvc-au: attribute 'version' has the value '2', not its fixed value '3'
bad-content.xml:3:5: cvc-complex-type.2.4: element 'author' is not allowed here; expected 'title'
bad-content.xml:4:5: cvc-complex-type.2.4: element 'title' is not allowed here; expected 'author' or 'date'
```

A missing element costs one report, and so does an extra one:

```console
$ sed '/<title abbrev/d' vane.xml | sxml validate --schema=vane.xsd -
-:10:5: cvc-complex-type.2.4: element 'seriesInfo' is not allowed here; expected 'title'
$ sed 's|<area>Applications</area>|<area>Applications</area><status>draft</status>|' vane.xml \
  | sxml validate --schema=vane.xsd -
-:21:30: cvc-complex-type.2.4: element 'status' is not allowed here; expected 'area', 'keyword' or 'abstract'
```

## Documents that are not well-formed

A document that is not well-formed can't be valid, but sxml still
reports the validation errors it found before the place where the
document breaks off. Here are the first twelve lines of the draft,
with a wrong version:

```console
$ head -12 vane.xml | sed 's/version="3"/version="three"/' > cut.xml
$ sxml validate --schema=vane.xsd cut.xml
cut.xml:7:6: cvc-datatype-valid.1.2.1: attribute 'version' has the value 'three', which is not valid for type 'xs:integer'
cut.xml:13:1: not well-formed: the document ends or breaks off before a construct is complete (XML 1.0, section 2.1)
```

## Lax validation

With `--mode=lax`, a document whose document element has no
declaration is not an error. sxml says that its validity is not known,
and exits with status 0. The editorial note of the chapter's opening
has no declaration in `vane.xsd`, which declares `cref` only inside
paragraphs:

```console
$ sxml validate --schema=vane.xsd --mode=lax xsd-dtd.xml; echo "exit status $?"
xsd-dtd.xml: validity not known: the schema has no declaration of the document element (lax assessment)
exit status 0
```

## Reports for programs

`--format=json` writes the same report as one JSON object, with the
status of the schema and of each file, and each error with its code,
line, column, byte offset and message:

```console
$ sxml validate --schema=vane.xsd --format=json bad-xref.xml
{"schema":{"documents":["vane.xsd"],"status":"valid"},"files":[{"file":"bad-xref.xml","status":"invalid","errors":[{"code":"cvc-identity-constraint.4.3","line":11,"column":27,"byte_offset":375,"message":"element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')"},{"code":"cvc-identity-constraint.4.3","line":12,"column":41,"byte_offset":452,"message":"element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')"}]}]}
$ sxml validate --schema=bad-schema.xsd --format=json xsd-dtd.xml
{"schema":{"documents":["bad-schema.xsd"],"status":"invalid","errors":[{"document":"bad-schema.xsd","code":"src-attribute","line":8,"column":7,"byte_offset":344,"message":"An attribute's value is not allowed: use"}]},"files":[]}
```

The exit status tells a script what happened without reading any of
it: 0 when every file is valid, 1 when the schema or a file is invalid
or not well-formed, 2 for a mistake on the command line, and 3 when a
file can't be read:

```console
$ sxml validate --schema=vane.xsd vane.xml bad-xref.xml > /dev/null 2>&1; echo "exit status $?"
exit status 1
$ sxml validate --schema=vane.xsd no-such-draft.xml; echo "exit status $?"
sxml validate: cannot read 'no-such-draft.xml'
exit status 3
```

From XQuery, a `validate` expression that fails raises `err:XQDY0027`
with the first message, which `try` and `catch` can handle; [Typed Data
and Schema-Aware Queries](xsd-typed-data.md) shows it. Programs that
embed sxml get each error as a structured value, with its constraint
and location: [Embedding sxml in C](c.md) shows the C interface, and
[sxml in JavaScript](javascript.md) the JavaScript one.
