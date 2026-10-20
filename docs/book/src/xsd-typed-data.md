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

A schema-aware stylesheet can also validate its *output*, with
`validation="strict"` on `xsl:result-document`: a transformation that
would write an invalid document fails instead.
