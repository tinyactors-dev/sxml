# Working with Text

<!-- examples: spec -->

Text is where software meets people, and people write in many languages.
This chapter covers three things sxml does with text beyond plain
strings: sorting and comparing it the way a language expects, searching
it by words instead of characters, and writing dates and numbers in
other languages. The examples run on the Latch draft, in
`docs/book/examples/spec`.

## Collations

By default, strings sort by their Unicode code points. That puts every
capital letter before every small letter, so the terms of the draft and
the words that refer to them sort into two runs:

```console
$ sxml xpath --context-item=latch.xml 'sort(distinct-values((//dt, //xref[starts-with(@target, "term-")]) ! string()))'
"Code"
"Controller"
"Lock"
"Lockout"
"code"
"controller"
"lock"
"lockout"
```

A *collation* is a set of rules for comparing strings. Every function and
instruction that compares strings (`sort`, `compare`, `distinct-values`,
`order by`, `xsl:sort`, `index-of` and more) can take one, named by a
URI. The Unicode Collation Algorithm (UCA) gives the order people
expect:

```console
$ sxml xpath --context-item=latch.xml 'sort(distinct-values((//dt, //xref[starts-with(@target, "term-")]) ! string()), "http://www.w3.org/2013/collation/UCA")'
"code"
"Code"
"controller"
"Controller"
"lock"
"Lock"
"lockout"
"Lockout"
```

Parameters in the URI tailor the order. `strength` says which
differences count: `primary` ignores accents and case, `secondary`
ignores only case. With it, `distinct-values` finds that the draft has
four terms, however a sentence capitalizes them:

```console
$ sxml xpath --context-item=latch.xml 'distinct-values((//dt, //xref[starts-with(@target, "term-")]) ! string(), "http://www.w3.org/2013/collation/UCA?strength=secondary")'
"Lock"
"Controller"
"Code"
"Lockout"
$ sxml xpath 'compare("Strasse", "Straße", "http://www.w3.org/2013/collation/UCA?strength=primary")'
0
```

`numeric=yes` sorts digits by their numeric value. The anchors of the
requirements end in numbers, and by code points `req-10` comes before
`req-2`:

```console
$ sxml xpath --context-item=latch.xml 'string-join(sort(//t/@anchor ! string()), " ")'
"req-1 req-10 req-11 req-2 req-3 req-4 req-5 req-6 req-7 req-8 req-9"
$ sxml xpath --context-item=latch.xml 'string-join(sort(//t/@anchor ! string(), "http://www.w3.org/2013/collation/UCA?numeric=yes"), " ")'
"req-1 req-2 req-3 req-4 req-5 req-6 req-7 req-8 req-9 req-10 req-11"
```

Punctuation counts, too. The authors of the references include
`M'Raihi`, and by default the apostrophe sorts before every letter, so
`M'Raihi` comes before `Machani`. `alternate=shifted` makes punctuation
and spaces count only when everything else is equal, as a reference
list or a phone book expects:

```console
$ sxml xpath --context-item=latch.xml 'string-join(sort(distinct-values(//reference//author/@surname), "http://www.w3.org/2013/collation/UCA"), ", ")'
"Bradner, Crocker, Leiba, M'Raihi, Machani, Overell, Pei, Rydell"
$ sxml xpath --context-item=latch.xml 'string-join(sort(distinct-values(//reference//author/@surname), "http://www.w3.org/2013/collation/UCA?alternate=shifted"), ", ")'
"Bradner, Crocker, Leiba, Machani, M'Raihi, Overell, Pei, Rydell"
```

`lang` uses the rules of a language. The draft is in English, but a
translation of its terms would not be. In Spanish, `ñ` is a letter of
its own, after `n`:

```console
$ sxml xpath 'sort(("llave", "luz", "cerradura", "código", "ñu", "nube"), "http://www.w3.org/2013/collation/UCA?lang=es")'
"cerradura"
"código"
"llave"
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

sxml carries the root collation of the Unicode CLDR, and the tailorings
for English, German, French, Italian, Spanish and Dutch with their
regional variants. Other languages use the root order.

## Full-text search

`contains` finds characters. Searching for *words* is a different
problem: "unlock" should find "unlocked", a phrase should match its
words in order, and two words should be near each other. *XQuery and
XPath Full Text* adds the `contains text` expression for this:

```console
$ sxml xpath --context-item=latch.xml '//t[@anchor][. contains text "unlock"]/@anchor/string()'
"req-4"
"req-5"
```

The search is by words and ignores case, so it found "unlock" in
"unlock request" and in "MUST NOT unlock", but not "unlocked": that is
another word. Options after `using` change that. With `stemming`, words
match by their stem:

```console
$ sxml xpath --context-item=latch.xml '//t[@anchor][. contains text "unlock" using stemming]/@anchor/string()'
"req-1"
"req-3"
"req-4"
"req-5"
"req-6"
```

Now the states "unlocked" of req-1, req-3 and req-6 match. A phrase in
quotes matches its words in order, and `ftand`, `ftor` and `ftnot`
combine searches:

```console
$ sxml xpath --context-item=latch.xml '//t[@anchor][. contains text "status request"]/@anchor/string()'
"req-6"
"req-10"
$ sxml xpath --context-item=latch.xml '//t[@anchor][. contains text "controller" ftand "lock"]/@anchor/string()'
"req-10"
"req-11"
$ sxml xpath --context-item=latch.xml 'string-join(//t[@anchor][. contains text "lock" ftand ftnot "controller"]/@anchor, " ")'
"req-1 req-2 req-3 req-4 req-5 req-6 req-7"
```

`distance` and `window` say how close words must be, and `wildcards`
makes a `.*` in a word match any characters, so `lock.*` finds "lockout"
as well:

```console
$ sxml xpath --context-item=latch.xml '//t[@anchor][. contains text "lock" ftand "lockout" distance at most 3 words]/@anchor/string()'
"req-7"
"req-11"
$ sxml xpath --context-item=latch.xml 'string-join(//t[@anchor][. contains text "lock.*" using wildcards]/@anchor, " ")'
"req-1 req-2 req-3 req-4 req-5 req-6 req-10 req-7 req-8 req-11"
```

Only req-9 has no word that starts with "lock"; req-8, "A lockout
SHOULD last five minutes", is found by the wildcard alone.

A `score` variable in a `for` clause gives each result a relevance
between 0 and 1, to rank the results:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    for $t score $score in //t[@anchor][. contains text "lockout" ftor "code"]
    order by $score descending
    return $t/@anchor || " " || round($score, 2)'
"req-4 0.26"
"req-5 0.18"
"req-7 0.18"
"req-1 0.17"
"req-11 0.17"
"req-8 0.17"
```

req-4 mentions "code" twice in a short paragraph and comes first.
`using language "de"` selects the language of the words, for stemming
and stop words. sxml has stemmers for English, German, French, Italian,
Spanish and Dutch, and reads lists of stop words and thesauri from
files. `contains text` works in XQuery as in XPath, as the last example
shows.

## Dates in other languages

`format-date`, `format-time` and `format-dateTime` write dates with a
*picture string*. Each part in square brackets is a component: `[D]` the
day, `[MNn]` the month's name, `[FNn]` the day of the week, `[Y]` the
year:

```console
$ sxml xpath "format-date(xs:date('2026-10-21'), '[FNn], [D] [MNn] [Y]')"
"Wednesday, 21 October 2026"
$ sxml xpath "format-date(xs:date('2026-10-21'), '[D1o] [MNn], [Y]')"
"21st October, 2026"
$ sxml xpath "format-date(xs:date('2026-10-21'), '[Y0001]-[M01]-[D01], week [W]')"
"2026-10-21, week 43"
```

The draft writes its date as three attributes, with the month as an
English name. A query makes an `xs:date` of it, and then writes it in
three languages: the third argument of `format-date` is a language, and
`n` instead of `Nn` writes a name in small letters, as French does:

`text-date.xq`:

```xquery
{{#include ../examples/spec/text-date.xq}}
```

```console
$ sxml xquery --method=adaptive --context-item=latch.xml text-date.xq
"21 October 2026"
"Mittwoch, 21. Oktober 2026"
"mercredi 21 octobre 2026"
$ sxml xquery --method=adaptive --context-item=latch-00.xml text-date.xq
"6 July 2026"
"Montag, 6. Juli 2026"
"lundi 6 juillet 2026"
```

The language can have a region, and the fourth argument is a calendar.
In Austria, January is "Jänner", as in the month of RFC 5234; `JE` is
the calendar of Japanese eras:

```console
$ sxml xpath "format-date(xs:date('2008-01-01'), '[MNn] [Y]', 'de-AT', (), ())"
"Jänner 2008"
$ sxml xpath "format-date(xs:date('2026-10-21'), '[E] [Y] [MNn] [D]', 'en', 'JE', ())"
"Reiwa 8 October 21"
```

The fifth argument is a *place*: a time zone name from the IANA Time
Zone Database, which sxml reads from `/usr/share/zoneinfo`. A lock in
New York that reports a lockout at 14:30 UTC reports it at 10:30 local
time:

```console
$ sxml xpath "format-dateTime(xs:dateTime('2026-10-21T14:30:00Z'), '[H01]:[m01] [ZN]', 'en', (), 'America/New_York')"
"10:30 EDT"
$ sxml xpath "civil-timezone(xs:dateTime('2026-07-06T12:00:00'), 'Europe/Lisbon')"
xs:dayTimeDuration("PT1H")
```

`civil-timezone` gives the offset of a place at a moment, summer time
included.

## Numbers in words

`format-integer` writes numbers as words, by the rules of the Unicode
CLDR for each language, and ordinals with `o`:

```console
$ sxml xpath "format-integer(11, 'w'), format-integer(11, 'w', 'de'), format-integer(11, 'Ww;o', 'fr')"
"eleven"
"elf"
"Onzième"
```

The draft writes its numbers in words: ten seconds, three codes, five
minutes. The lock that implements it has them as numbers. A query can
check that each requirement says the number the lock uses, with
`format-integer` making the word and `contains text` finding it (an
expression in braces is searched for):

`text-numbers.xq`:

```xquery
{{#include ../examples/spec/text-numbers.xq}}
```

```console
$ sxml xquery --method=adaptive --context-item=latch.xml text-numbers.xq
"req-3 states ten: true"
"req-7 states three: true"
"req-8 states five: true"
$ sxml xquery --method=adaptive --context-item=latch-00.xml text-numbers.xq
"req-3 states ten: true"
"req-7 states three: false"
"req-8 states five: false"
```

The first draft said "five" rejected codes and "one minute"; the
second agrees with the lock.

`format-number` writes decimals with grouping and a fixed number of
places. Its options choose the separators:

```console
$ sxml xpath "format-number(5 div 11, '0.0%')"
"45.5%"
$ sxml xpath "format-number(300000, '#,##0'), format-number(300000, '#.##0', { 'decimal-separator': ',', 'grouping-separator': '.' })"
"300,000"
"300.000"
```

Five of the eleven requirements are MUSTs, and a lockout of five
minutes is 300,000 milliseconds, or 300.000 in German. In XSLT,
`xsl:number` and `xsl:sort` take a `lang` attribute and use the same
data.
