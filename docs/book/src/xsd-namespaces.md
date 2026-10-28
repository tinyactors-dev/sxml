# Namespaces and Larger Schemas

<!-- examples: scxml -->

The draft's vocabulary has no namespace. Most real vocabularies have
one: XHTML, SVG, Atom, and SCXML, the W3C's language for state
machines. This section, and the rest of the chapter, works in the
`scxml` directory, on the Vane feature flag written as an SCXML
machine, `flag.scxml`. [Project: Self-Describing State Machines](scxml.md) explains
the machine itself; here it is a document with three namespaces in it:

- SCXML's, `http://www.w3.org/2005/07/scxml`, for the states and
  transitions;
- `urn:example:vane`, with the prefix `vane`, for the annotations
  that describe the machine's events and data;
- XHTML's, `http://www.w3.org/1999/xhtml`, for the documentation of
  each state and event.

Here is its start and one state:

```xml
{{#include ../examples/scxml/flag.scxml:1:6}}
```

```xml
{{#include ../examples/scxml/flag.scxml:43:51}}
```

Two schemas describe it: `scxml.xsd` for SCXML, and `vane-data.xsd`
for the annotations. XHTML has no schema here, and the machine is
valid all the same:

```console
$ sxml validate --schema=scxml.xsd flag.scxml; echo "exit status $?"
exit status 0
```

This section shows how a schema describes a namespace, how two schemas
for two namespaces work together, and how a schema leaves room for
namespaces it doesn't know.

## A target namespace

`targetNamespace` on `xs:schema` puts every global declaration and
named type of the schema into that namespace. Here is the top of
`scxml.xsd`:

```xml
{{#include ../examples/scxml/scxml.xsd:10:15}}
```

Inside the schema, references to its own components are QNames in the
target namespace. `scxml.xsd` binds the prefix `sc` to it, so a
reference to its own group or type is written with the prefix:

```xml
{{#include ../examples/scxml/scxml.xsd:58:60}}
```

The other way is to make the target namespace the schema's default
namespace, with `xmlns="http://www.w3.org/2005/07/scxml"`; then
`ref="executable"` finds it. Either way, a reference without the right
namespace names nothing, and the schema doesn't compile. Built-in types
keep their `xs:` prefix.

`elementFormDefault="qualified"` puts the *local* elements, such as
`state` and `transition`, into the target namespace as well. Without
it, only global elements are in the namespace, and local elements are
in no namespace, which surprises almost everyone; most schemas say
`qualified`. Attributes are different: unprefixed attributes such as
`id` and `event` are in no namespace, and `attributeFormDefault` stays
at its default, `unqualified`.

## Namespace mistakes

The same machine without its namespace has an element that the schema
doesn't know at all:

```console
$ sed 's| xmlns="http://www.w3.org/2005/07/scxml"||' flag.scxml | sxml validate --schema=scxml.xsd -
-:2:1: cvc-elt.1: no global declaration governs element 'scxml'
```

When `cvc-elt.1` says no declaration governs an element that you can
see in the schema, check its namespace first.

A namespace declaration that is missing lower down moves elements into
the wrong namespace. Without its `xmlns`, the first state's `div` is in
the default namespace of its parent, SCXML's, and SCXML has no `div`:

```console
$ sed '0,/<div xmlns="http:\/\/www.w3.org\/1999\/xhtml">/s//<div>/' flag.scxml \
  | sxml validate --schema=scxml.xsd -
-:44:5: cvc-complex-type.2.4: element 'div' is not allowed here; expected 'onentry', 'onexit', 'transition', 'state', 'final', 'datamodel' or an element of another namespace
```

An element's namespace comes from the namespace declarations in scope
where it is written, never from what it means.

## Import

A schema can be split into several documents.

- `xs:include` brings in a schema document with the *same* target
  namespace (or none, in which case its components take the including
  schema's namespace). The draft's `levels.xsd` includes `vane.xsd`
  this way.
- `xs:import` makes the components of *another* namespace available.
  The vane annotations have their own schema, `vane-data.xsd`, with
  the target namespace `urn:example:vane`, and `scxml.xsd` imports it:

```xml
{{#include ../examples/scxml/scxml.xsd:17}}
```

`vane-data.xsd` declares the annotation attributes as *global*
attributes, so that they can be used on elements of other namespaces:

```xml
{{#include ../examples/scxml/vane-data.xsd:93:100}}
```

`schemaLocation` is relative to the document that contains it. It's
only a hint on `xs:import`: the components can also come from another
schema document given on the command line.

## Wildcards and lax processing

Nothing in `scxml.xsd` mentions XHTML or the vane attributes by name.
Instead, almost every type has wildcards:

```xml
<xs:any namespace="##other" processContents="lax"/>
<xs:anyAttribute namespace="##other" processContents="lax"/>
```

`xs:any` allows an element, and `xs:anyAttribute` an attribute, of the
namespaces it names. `##other` means every namespace except the target
namespace (and except no namespace); others are `##any`, `##local`,
`##targetNamespace`, or a list of namespace names. `processContents`
says what happens to what the wildcard lets in:

- `strict`: it must have a declaration, and be valid by it;
- `lax`: it is validated when the schema has a declaration for it, and
  let through when not;
- `skip`: it is not validated at all.

So the vane attributes, which `vane-data.xsd` declares, are
validated, and the XHTML, which no schema declares, is let through. A
machine with an anchor that is not a name, a type given without its
prefix, and an XHTML element that XHTML doesn't have, shows the
difference:

```console
$ sed -e 's/vane:implements="req-5 req-8"/vane:implements="req-5 8"/' \
      -e 's/vane:type="vane:seconds"/vane:type="seconds"/' \
      -e 's|<p>The flag is archived.|<p><blink>The flag</blink> is archived.|' flag.scxml \
  | sxml validate --schema=scxml.xsd -
-:80:22: cvc-datatype-valid.1.2.1: attribute 'vane:implements' has the value 'req-5 8', which is not valid for an anonymous type derived from 'xs:anySimpleType'
```

Only the anchor is reported. `8` is not an `xs:NCName`, so it can't be
an item of the list. `seconds` is a valid `xs:QName`, the name `seconds`
in the default namespace, SCXML's; that it names no type is a mistake
the schema can't see, and [Checking What the Schema
Cannot](scxml-checks.md) finds it. `blink` is not checked at all.

Lax processing has a price: when a declaration is missing, nothing is
validated and nothing is said. Without the `schemaLocation` of the
import, `scxml.xsd` doesn't find `vane-data.xsd`, and the broken
anchor goes through:

```console
$ sed 's| schemaLocation="vane-data.xsd"||' scxml.xsd > scxml-alone.xsd
$ sed 's/vane:implements="req-5 req-8"/vane:implements="req-5 8"/' flag.scxml > broken.scxml
$ sxml validate --schema=scxml-alone.xsd broken.scxml; echo "exit status $?"
exit status 0
$ sxml validate --schema=scxml-alone.xsd --schema=vane-data.xsd broken.scxml
broken.scxml:80:22: cvc-datatype-valid.1.2.1: attribute 'vane:implements' has the value 'req-5 8', which is not valid for an anonymous type derived from 'xs:anySimpleType'
```

Every `--schema` adds documents to one schema, which is how you combine
vocabularies that don't import each other.

## Schema hints in documents

Documents often name their schema with `xsi:schemaLocation` (pairs of a
namespace and a location) or `xsi:noNamespaceSchemaLocation` (a location
for names in no namespace). Here is the broken machine with such a hint
on its document element:

```console
$ sed 's|<scxml xmlns="http://www.w3.org/2005/07/scxml"|& xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.w3.org/2005/07/scxml scxml.xsd"|' broken.scxml > hinted.scxml
$ head -2 hinted.scxml
<?xml version="1.0" encoding="UTF-8"?>
<scxml xmlns="http://www.w3.org/2005/07/scxml" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.w3.org/2005/07/scxml scxml.xsd"
```

By default `sxml validate` doesn't follow these hints: a document
doesn't get to choose the schema it's checked against, so you name the
schema with `--schema`. Without one, there is nothing to validate
against:

```console
$ sxml validate hinted.scxml
sxml validate: missing option '--schema' (or give --use-schema-location or --schematron)
Usage: sxml validate [--schema=SCHEMA]... [--schematron=SCH] [--phase=PHASE] [--schematron-param=NAME=VALUE]... [--use-schema-location] [--mode=MODE] [--format=FORMAT] FILE...
Try 'sxml validate --help'.
```

`--use-schema-location` asks for the hints to be followed. The hinted
documents are local files, relative to the document that names them,
and they join the `--schema` documents, if you give any, in one schema
for that document:

```console
$ sxml validate --use-schema-location hinted.scxml
hinted.scxml:80:22: cvc-datatype-valid.1.2.1: attribute 'vane:implements' has the value 'req-5 8', which is not valid for an anonymous type derived from 'xs:anySimpleType'
$ sxml validate --use-schema-location --schema=scxml.xsd hinted.scxml
hinted.scxml:80:22: cvc-datatype-valid.1.2.1: attribute 'vane:implements' has the value 'req-5 8', which is not valid for an anonymous type derived from 'xs:anySimpleType'
```

The hinted copy is the broken machine, so both runs find its anchor;
the second takes SCXML's schema from `--schema`. A hint only adds a
namespace the schema doesn't have yet: with
`--schema=scxml.xsd`, the hint for SCXML's namespace is ignored, so a
document can't swap the schema you chose for one of its own. A hinted
file that doesn't exist isn't an error by itself (XSD 1.1 Part 1,
4.3.2); the elements it would have declared are then simply undeclared.

## Namespaces in queries

In XQuery, `import schema` binds a prefix to the schema's namespace, or
makes it the default element namespace. Then element names and type
names work as in the schema. The imported `vane-data.xsd` comes along
with `scxml.xsd`, so its declarations are known; `declare namespace`
only binds the prefix:

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace vane = "urn:example:vane";
    let $machine := validate { / }
    return (
      count($machine//sc:state[. instance of element(*, sc:state)]),
      $machine//sc:state ! `{@id}: {count(data(@vane:implements))}`
    )'
5
"stable: 1"
"checking: 1"
"rolling-out: 1"
"paused: 2"
"archived: 0"
```

`data(@vane:implements)` is a sequence of names, because the
attribute's type is a list. Without a prefix, a name in a path means no
namespace, and finds nothing in this document:

```console
$ sxml xquery --context-item=flag.scxml --expression='count(//state)'
0
```
