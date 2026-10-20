# Typed Data and Schema-Aware Queries

<!-- examples: xsd -->

Validation does more than say yes or no. A validated document carries
the types of its schema: each element and attribute is *annotated* with
its type, and its *typed value* is a number, a date or whatever the type
says, instead of untyped text. XPath, XQuery and XSLT can use those
types. Processors that can do this are called *schema-aware*, and sxml
is one.

## Typed values in XQuery

`import schema` brings a schema's declarations into a query, and
`validate` validates a document (or element) and returns a typed copy:

`typed.xq`:

```xquery
{{#include ../examples/xsd/typed.xq}}
```

```console
$ sxml xquery --method=adaptive --context-item=library.xml typed.xq
"The Art of Computer Programming: 360"
"Dune: 19.98"
"The Left Hand of Darkness: 23"
```

There are no conversions in the query. `@year` is an `xs:gYear`, so it
compares with a year using `lt`; `price` is a decimal, so `price * 2`
is exact decimal arithmetic. Compare that with Chapter 2, where
`sum(//book/@pages)` on the untyped document gave a double.

The typed values have the types of the schema, including the ones you
defined:

```console
$ sxml xquery --method=adaptive --context-item=library.xml --expression='
    import schema default element namespace "" at "library.xsd";
    let $library := validate { / }
    return (
      sum($library//price),
      type-of(data(($library//price)[1])),
      $library//book[@pages > 600]/@id/string()
    )'
278.99
"Q{}amount"
"geb"
"taocp"
"sicp"
```

`Q{}amount` is the type `amount` in no namespace, written as an
*expanded name*. When the document is not valid, `validate` raises an
error with the same message `sxml validate` gives:

```console
$ sxml xquery --context-item=damaged.xml --expression='
    import schema default element namespace "" at "library.xsd";
    validate { / }'
expression:3:5: err:XQDY0027: cvc-enumeration-valid: attribute 'currency' has the value 'GBP', which is not valid for type 'currency': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'EUR', 'USD'
```

## Validating in a query

`validate` has three modes:

- `validate { E }` or `validate strict { E }`: the element must have a
  global declaration, and must be valid by it;
- `validate lax { E }`: elements without a declaration are let through,
  and the ones with a declaration are validated wherever they are;
- `validate type T { E }`: the element is validated against the type
  `T`, whatever its name.

`loan.xsd`, from the start of this chapter, declares `loan`.
Inside an element it doesn't know, lax validation still finds the loan
and types it:

```console
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "loan.xsd";
    let $loans := validate lax {
      <loans><loan book="dune" from="2026-10-01" until="2026-10-22"/></loans>
    }
    return $loans/loan/@until - $loans/loan/@from'
xs:dayTimeDuration("P21D")
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "loan.xsd";
    validate { <loans/> }'
expression:3:5: err:XQDY0084: Strict validation of an element whose name has no global element declaration among the in-scope schema definitions (XQuery 4.0, 4.25)
```

A `validate` expression can't be followed directly by a path or `!`;
put it in parentheses, `(validate { / })//book`, as the examples here
do.

`validate type` works for an element that has no global declaration of
its own. `price` is declared only inside `book`, but its type is named:

```console
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "library.xsd";
    (validate type price { <price currency="USD">3.50</price> }) ! (data(.) * 2)'
7
```

An invalid input raises `err:XQDY0027`, which a query can catch, for
example to check several files and go on:

```console
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "library.xsd";
    for $file in ("library.xml", "damaged.xml")
    return try {
      (validate { doc($file) }) ! ($file || ": valid")
    } catch err:XQDY0027 {
      $file || ": " || substring-before($err:description, ":")
    }'
"library.xml: valid"
"damaged.xml: cvc-enumeration-valid"
```

The description is the message of the first error that validation
found.

## Values of your own types

Every named simple type of an imported schema has a constructor
function, as the built-in types do, and `cast as` and `castable as`
work with it. The facets are checked:

```console
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "library.xsd";
    amount("3.45") + 1, "3.456" castable as amount, currency("EUR")'
4.45
false()
"EUR"
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "library.xsd";
    currency("GBP")'
expression:3:5: err:FORG0001: cvc-enumeration-valid: the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4)
```

## Elements without a typed value

The typed value of an element with simple content, such as `price`, is
its value. An element whose type has *element-only* content, such as
`book`, has no typed value at all, and asking for it is an error:

```console
$ sxml xquery --method=adaptive --context-item=library.xml --expression='
    import schema default element namespace "" at "library.xsd";
    data(((validate { / })//book)[1])'
expression:3:5: err:FOTY0012: An element whose type has element-only content has no typed value (XDM 4.0, section 7.5.2.4)
```

That also catches a common mistake: `$book = "Dune"` compares the typed
value of `$book`, so it raises this error for a validated book. On an
untyped document it compares all the text of the book, which is rarely
what you meant either. Compare `$book/title` instead. Elements of
mixed content have their text as an untyped value.

## Types in signatures

With a schema imported, sequence types can name its types.
`element(price, price)` is a `price` element validated as the type
`price`. A function with that parameter type accepts only validated
prices, which catches a whole class of mistakes:

```console
$ sxml xquery --method=adaptive --context-item=library.xml --expression='
    import schema default element namespace "" at "library.xsd";
    declare function local:euros($price as element(price, price)) as xs:decimal {
      if ($price/@currency = "USD") then $price * 0.92 else data($price)
    };
    (validate { / })//price ! local:euros(.)'
20.7
165.6
50.6
9.99
11.5
```

Pass it the prices of the unvalidated document, and the call fails:

```console
$ sxml xquery --context-item=library.xml --expression='
    import schema default element namespace "" at "library.xsd";
    declare function local:euros($price as element(price, price)) as xs:decimal {
      data($price)
    };
    //price ! local:euros(.)'
expression:6:15: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The sequence types that use a schema are:

- `element(N, T)`: an element named `N` whose type is `T` or derived
  from it; `element(*, T)` for any name;
- `attribute(N, T)` and `attribute(*, T)`, the same for attributes;
- `schema-element(N)`: an element that the global declaration `N`
  governs, or a member of its substitution group;
- `schema-attribute(N)`: the same for a global attribute declaration.

```console
$ sxml xquery --method=adaptive --context-item=library.xml --expression='
    import schema default element namespace "" at "library.xsd";
    let $library := validate { / }
    return (
      ($library//book)[1] instance of element(*, book),
      ($library//@year)[1] instance of attribute(*, xs:gYear),
      $library/library instance of schema-element(library),
      /library instance of schema-element(library)
    )'
true()
true()
true()
false()
```

The last one is false because the context item, the document as it was
parsed, was never validated: its elements are untyped.

## Typed copies

When a constructor copies validated nodes into a new element, the
copies keep their types only with `declare construction preserve`.
With `strip`, they become untyped, as if they came from a document
without a schema:

```console
$ sxml xquery --method=adaptive --context-item=library.xml --expression='
    import schema default element namespace "" at "library.xsd";
    declare construction preserve;
    let $library := validate { / }
    return <cheap>{ $library//price[. lt 20] }</cheap>/price instance of element(*, price)+'
true()
$ sxml xquery --method=adaptive --context-item=library.xml --expression='
    import schema default element namespace "" at "library.xsd";
    declare construction strip;
    let $library := validate { / }
    return <cheap>{ $library//price[. lt 20] }</cheap>/price instance of element(*, price)+'
false()
```

## Schema-aware XSLT

In XSLT, `xsl:import-schema` imports a schema, and the `validation`
attribute of `xsl:document`, `xsl:copy-of`, `xsl:result-document` and
other instructions validates what they make. Patterns and sequence types
can test types:

`typed.xsl`:

```xml
{{#include ../examples/xsd/typed.xsl}}
```

```console
$ sxml transform typed.xsl library.xml
Godel, Escher, Bach: 23.5
The Art of Computer Programming: 181
Structure and Interpretation of Computer Programs: 56
Dune: 10.99
The Left Hand of Darkness: 12.5
```

Here is a larger stylesheet that uses more of it: a typed stylesheet
parameter, patterns with schema types, and new elements that are
validated as they are made:

`prices.xsl`:

```xml
{{#include ../examples/xsd/prices.xsl}}
```

- `xsl:copy-of` with `validation="strict"` validates the copy, and the
  variable's type, `document-node(schema-element(library))`, says that
  it must be a validated library.
- `match="schema-element(library)"` and `match="element(*, book)"`
  match by declaration and by type, not only by name.
- The parameter `rate` has the type `xs:decimal`, so `--param`, which
  passes text, gives it a decimal, and `price * $rate` is exact.
- `xsl:element` with `type="price"` validates the new element against
  the type `price` as it is made.

```console
$ sxml transform --param=rate=1.234 prices.xsl library.xml
<?xml version="1.0" encoding="UTF-8"?>
<prices>
  <price currency="USD">27.77</price>
  <price currency="USD">222.12</price>
  <price currency="USD">67.87</price>
  <price currency="EUR">12.33</price>
  <price currency="EUR">14.19</price>
</prices>
```

Without the `round`, the new prices have three decimals, which the
type `amount` doesn't allow, and the transformation stops with a type
error instead of writing them:

```console
$ sed 's/round(price \* \$rate, 2)/price * $rate/' prices.xsl > unrounded.xsl
$ sxml transform --param=rate=1.234 unrounded.xsl library.xml
unrounded.xsl:28:5: err:XTTE1540: cvc-fractionDigits-valid: element 'price' has the value '27.765', which is not valid for type 'amount': the value has more fraction digits than the fractionDigits facet (XSD 1.1 Part 2, 4.3.12.4); the fractionDigits facet is '2'
```

And an input that isn't valid is stopped at the door, by the
`xsl:copy-of`:

```console
$ sxml transform prices.xsl damaged.xml
prices.xsl:14:7: err:XTTE1510: cvc-enumeration-valid: attribute 'currency' has the value 'GBP', which is not valid for type 'currency': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'EUR', 'USD'
```

A schema-aware stylesheet can also validate its *output*, with
`validation="strict"` on `xsl:result-document`: a transformation that
would write an invalid document fails instead.

## From C and JavaScript

Programs validate through the library's schema interface: compile a
schema once, then validate many documents with it, and receive each
error as a value with its constraint and location. Chapter 9 shows the
pattern in C (`sxml/schema.h`), and Chapter 10 in JavaScript, where
`compileSchema` makes a schema whose `validate` returns a report of the
validity and the errors.
