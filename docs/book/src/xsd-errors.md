# Reading Validation Errors

<!-- examples: xsd -->

Validation errors are meant to be read by people, and acted on. This
section explains what sxml's messages contain, lists the ones you'll
meet most with their usual fixes, and shows how programs read the same
report as JSON.

## The parts of a message

Every message has the same parts:

```text
damaged.xml:6:14: cvc-enumeration-valid: attribute 'currency' has the value 'GBP', ...
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
`cos-` for rules about components, and others. This schema has three
mistakes:

`broken.xsd`:

```xml
{{#include ../examples/xsd/broken.xsd}}
```

```console
$ sxml validate --schema=broken.xsd one-loan.xml; echo "exit status $?"
broken.xsd:8:7: src-attribute: An attribute's value is not allowed: use
exit status 1
```

`use` allows `optional`, `required` and `prohibited`. sxml first reads
the schema documents, then resolves the references between them, and
reports the errors of one step before it goes to the next, so fix what
it reports and run it again:

```console
$ sed 's/use="always"/use="required"/' broken.xsd > fixed.xsd
$ sxml validate --schema=fixed.xsd one-loan.xml
fixed.xsd:5:9: src-resolve: A QName value names no component of its kind: Q{http://www.w3.org/2001/XMLSchema}NCname
fixed.xsd:6:9: src-resolve: A QName value names no component of its kind: member-id
```

`src-resolve` means a name in the schema points at nothing: here a
misspelled built-in type (it's `xs:NCName`) and a type that was never
declared. The name is shown as an expanded name, with its namespace in
braces, so you can see whether a prefix went wrong too.

## Errors in documents

The constraints you'll see most in documents, and what they usually
mean:

| Constraint | Meaning | Usual fix |
| --- | --- | --- |
| `cvc-elt.1` | No global declaration for the document element | Check the element's namespace, and that the right schema is given |
| `cvc-complex-type.2.4` | An element is not allowed here, or the content is incomplete | Look at the expected elements it lists: order, spelling, namespace |
| `cvc-complex-type.2.1`, `2.3` | Text where the content is empty or element-only | Remove the text, or make the type mixed |
| `cvc-complex-type.3.2.2` | An attribute that the type doesn't declare | Declare it, or remove it |
| `cvc-complex-type.4` | A required attribute is missing | Add it |
| `cvc-datatype-valid.1.2.1` | The text is not a value of the type | Check the lexical form: `2026-10-20`, not `20.10.2026` |
| `cvc-pattern-valid`, `cvc-enumeration-valid`, `cvc-maxLength-valid`, ... | A value breaks a facet; the message names it | Compare the value with the facet it shows |
| `cvc-au` | A fixed attribute has another value | Use the fixed value |
| `cvc-type.2` | An element has an abstract type | Give a derived type with `xsi:type` |
| `cvc-elt.4.3` | `xsi:type` names a type not derived from the declared one | Use a derived type |
| `cvc-id.2` | An `xs:ID` value is used twice | Make the ids unique |
| `cvc-identity-constraint.4.1`, `4.2.2` | Two elements have the same key | Make the keys unique |
| `cvc-identity-constraint.4.3` | A keyref names a key that doesn't exist | Add the target, or fix the reference |
| `cvc-assertion`, `cvc-assertions-valid` | An assertion is false | Read the assertion in the message |

## One mistake can hide others

When an element is not allowed where it is, sxml can't know what the
author meant, so it reports that element and doesn't check the rest of
the content of its parent. Here `author` and `title` are swapped, and
the price is wrong too:

`late.xml`:

```xml
{{#include ../examples/xsd/late.xml}}
```

```console
$ sxml validate --schema=library.xsd late.xml
late.xml:4:7: cvc-complex-type.2.4: element 'author' is not allowed here; expected 'title'
```

Only the first mistake is reported. After you swap the two elements,
validation goes on past them, and the next run reports the currency
and the price. The other elements of the document, outside this book,
are checked as usual.

## Documents that are not well-formed

A document that is not well-formed can't be valid, but sxml still
reports the validation errors it found before the place where the
document breaks off:

```console
$ printf '<library>\n  <shelf name="Fiction">\n    <book id="dune" year="soon">\n' > cut.xml
$ sxml validate --schema=library.xsd cut.xml
cut.xml:3:21: cvc-datatype-valid.1.2.1: attribute 'year' has the value 'soon', which is not valid for type 'xs:gYear'
cut.xml:4:1: not well-formed: the document ends or breaks off before a construct is complete (XML 1.0, section 2.1)
```

## Lax validation

With `--mode=lax`, a document whose document element has no
declaration is not an error. sxml says that its validity is not known,
and exits with status 0:

```console
$ sxml validate --schema=library.xsd --mode=lax member.xml; echo "exit status $?"
member.xml: validity not known: the schema has no declaration of the document element (lax assessment)
exit status 0
```

## Reports for programs

`--format=json` writes the same report as one JSON object, with the
status of the schema and of each file, and each error with its code,
line, column, byte offset and message:

```console
$ sxml validate --schema=library.xsd --format=json late.xml
{"schema":{"documents":["library.xsd"],"status":"valid"},"files":[{"file":"late.xml","status":"invalid","errors":[{"code":"cvc-complex-type.2.4","line":4,"column":7,"byte_offset":74,"message":"element 'author' is not allowed here; expected 'title'"}]}]}
$ sxml validate --schema=broken.xsd --format=json one-loan.xml
{"schema":{"documents":["broken.xsd"],"status":"invalid","errors":[{"document":"broken.xsd","code":"src-attribute","line":8,"column":7,"byte_offset":255,"message":"An attribute's value is not allowed: use"}]},"files":[]}
```

The exit status tells a script what happened without reading any of
it: 0 when every file is valid, 1 when the schema or a file is invalid
or not well-formed, 2 for a mistake on the command line, and 3 when a
file can't be read:

```console
$ sxml validate --schema=library.xsd missing.xml; echo "exit status $?"
sxml validate: cannot read 'missing.xml'
exit status 3
```

From XQuery, a `validate` expression that fails raises `err:XQDY0027`
with the first message, which `try` and `catch` can handle; the next
section shows it. Programs that embed sxml get each error as a
structured value, with its constraint and location: Chapter 9 shows the
C interface, and Chapter 10 the JavaScript one.
