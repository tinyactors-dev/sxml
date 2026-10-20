# Working with Text

<!-- examples: text -->

Text is where software meets people, and people write in many languages.
This chapter covers three things sxml does with text beyond plain
strings: sorting and comparing it the way a language expects, searching
it by words instead of characters, and writing dates and numbers in
other languages. The examples are in `docs/book/examples/text`.

## Collations

By default, strings sort by their Unicode code points. That puts every
capital letter before every small letter, and accented letters after
`z`:

```console
$ sxml xpath 'sort(("peach", "Péché", "pêche", "Apple", "apple", "zebra"))'
"Apple"
"Péché"
"apple"
"peach"
"pêche"
"zebra"
```

A *collation* is a set of rules for comparing strings. Every function and
instruction that compares strings (`sort`, `compare`, `distinct-values`,
`order by`, `xsl:sort`, `index-of` and more) can take one, named by a
URI. The Unicode Collation Algorithm (UCA) gives the order people
expect:

```console
$ sxml xpath 'sort(("peach", "Péché", "pêche", "Apple", "apple", "zebra"), "http://www.w3.org/2013/collation/UCA")'
"apple"
"Apple"
"peach"
"Péché"
"pêche"
"zebra"
```

Parameters in the URI tailor the order. `lang` uses the rules of a
language. In Spanish, `ñ` is a letter of its own, after `n`:

```console
$ sxml xpath 'sort(("llama", "luz", "chico", "cosa", "ñu", "nube"), "http://www.w3.org/2013/collation/UCA?lang=es")'
"chico"
"cosa"
"llama"
"luz"
"nube"
"ñu"
```

German has two orders. In the phone book order, `ü` sorts as `ue`:

```console
$ sxml xpath 'sort(("Müller", "Mueller", "Muller"), "http://www.w3.org/2013/collation/UCA?lang=de")'
"Mueller"
"Muller"
"Müller"
$ sxml xpath 'sort(("Müller", "Mueller", "Muller"), "http://www.w3.org/2013/collation/UCA?lang=de-u-co-phonebk")'
"Mueller"
"Müller"
"Muller"
```

`numeric=yes` sorts digits by their numeric value, and `strength` says
which differences count: `primary` ignores accents and case,
`secondary` ignores only case:

```console
$ sxml xpath 'sort(("item 10", "item 9", "item 100"), "http://www.w3.org/2013/collation/UCA?numeric=yes")'
"item 9"
"item 10"
"item 100"
$ sxml xpath 'compare("Strasse", "Straße", "http://www.w3.org/2013/collation/UCA?strength=primary")'
0
```

sxml carries the root collation of the Unicode CLDR, and the tailorings
for English, German, French, Italian, Spanish and Dutch with their
regional variants. Other languages use the root order.

## Full-text search

`contains` finds characters. Searching for *words* is a different
problem: "parse" should find "parsing", "Parser" should match "parser",
and two words should be near each other. *XQuery and XPath Full Text*
adds the `contains text` expression for this:

`reviews.xml`:

```xml
{{#include ../examples/text/reviews.xml}}
```

```console
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "parser"]/@id/string()'
"1"
"4"
```

The search is by words and ignores case, so it found "parser" and
"Parser" but would not match "parsers". Options after `using` change
that. With `stemming`, words match by their stem:

```console
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "files" using stemming]/@id/string()'
"2"
```

`ftand`, `ftor` and `ftnot` combine searches, a phrase in quotes matches
the words in order, and `distance` and `window` say how close words must
be:

```console
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "memory" ftand "low" distance at most 3 words]/@id/string()'
"5"
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "fast" ftand ftnot "memory"]/@id/string()'
"2"
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "stream.*" using wildcards]/@id/string()'
"3"
```

A `score` variable in a `for` clause gives each result a relevance
between 0 and 1, to rank the results:

```console
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='
    for $review score $score in //review[. contains text "memory" ftor "fast"]
    order by $score descending
    return $review/@id || " " || round($score, 2)'
"5 0.32"
"3 0.19"
"2 0.19"
```

`using language "de"` selects the language of the words, for stemming
and stop words. sxml has stemmers for English, German, French, Italian,
Spanish and Dutch, and reads lists of stop words and thesauri from files.
`contains text` works in XPath as well:

```console
$ sxml xpath --context-item=reviews.xml '//review[. contains text "memory"]/@id/string()'
"3"
"5"
```

## Dates in other languages

`format-date`, `format-time` and `format-dateTime` write dates with a
*picture string*. Each part in square brackets is a component: `[D]` the
day, `[MNn]` the month's name, `[FNn]` the day of the week, `[Y]` the
year:

```console
$ sxml xpath "format-date(xs:date('2026-10-20'), '[FNn], [D] [MNn] [Y]')"
"Tuesday, 20 October 2026"
$ sxml xpath "format-date(xs:date('2026-10-20'), '[D1o] [MNn], [Y]')"
"20th October, 2026"
$ sxml xpath "format-date(xs:date('2026-10-20'), '[Y0001]-[M01]-[D01], week [W]')"
"2026-10-20, week 43"
```

The third argument is a language, the fourth a calendar:

```console
$ sxml xpath "format-date(xs:date('2026-10-20'), '[FNn], [D]. [MNn] [Y]', 'de', (), ())"
"Dienstag, 20. Oktober 2026"
$ sxml xpath "format-date(xs:date('2026-01-20'), '[D]. [MNn] [Y]', 'de-AT', (), ())"
"20. Jänner 2026"
$ sxml xpath "format-date(xs:date('2026-10-20'), '[E] [Y] [MNn] [D]', 'en', 'JE', ())"
"Reiwa 8 October 20"
```

In Austria, January is "Jänner". `JE` is the calendar of Japanese eras.
The fifth argument is a *place*: a time zone name from the IANA Time
Zone Database, which sxml reads from `/usr/share/zoneinfo`:

```console
$ sxml xpath "format-dateTime(xs:dateTime('2026-10-20T14:30:00Z'), '[H01]:[m01] [ZN]', 'en', (), 'America/New_York')"
"10:30 EDT"
$ sxml xpath "civil-timezone(xs:dateTime('2026-07-01T12:00:00'), 'Europe/Lisbon')"
xs:dayTimeDuration("PT1H")
```

`civil-timezone` gives the offset of a place at a moment, summer time
included.

## Numbers in words

`format-integer` writes numbers as words, by the rules of the Unicode
CLDR for each language, and ordinals with `o`:

```console
$ sxml xpath "format-integer(42, 'w'), format-integer(21, 'w', 'es'), format-integer(3, 'Ww;o', 'fr')"
"forty-two"
"veintiuno"
"Troisième"
```

`format-number` writes decimals with grouping and a fixed number of
places. Its options choose the separators:

```console
$ sxml xpath "format-number(1234567.891, '#,##0.00')"
"1,234,567.89"
$ sxml xpath "format-number(1234567.891, '#.##0,00', { 'decimal-separator': ',', 'grouping-separator': '.' })"
"1.234.567,89"
```

In XSLT, `xsl:number` and `xsl:sort` take a `lang` attribute and use the
same data.
