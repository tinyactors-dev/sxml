# Compact XML with EXI

<!-- examples: exi -->

XML text is easy to read and expensive to move. A feed of forty orders,
`orders.xml` in `docs/book/examples/exi`, is 13,732 bytes, and most of
them are the same names written again and again: `<order`, `</line>`,
`quantity=`. EXI, the Efficient XML Interchange format of the W3C,
writes the same document as a stream of events in which a name is
written once and referred to by a small number after that, and in which
each event takes only the bits that tell it apart from the events that
could have come instead. sxml reads and writes EXI 1.0 in all its
forms, and Canonical EXI, which gives one stream for one document.

## Encoding and decoding

`sxml encode` writes the EXI stream of an XML document, and `sxml
decode` writes the XML text of an EXI stream:

```console
$ sxml encode --output=orders.exi orders.xml
$ sxml xpath '("orders.xml", "orders.exi") ! `{.}: {bin:length(unparsed-binary(.))} bytes`'
"orders.xml: 13732 bytes"
"orders.exi: 2390 bytes"
$ sxml decode orders.exi | head -4
<orders date="2026-11-27">
  <order id="A-1001" status="shipped">
    <customer><name>Frances Allen</name><city>Lyon</city><country>FR</country></customer>
    <line sku="KB-104" quantity="3" price="89.00">Mechanical keyboard</line>
```

The stream is a sixth of the text, and nothing was compressed yet: the
saving is all in how the events are written. Without `--output`, both
commands write to standard output, so they fit in a pipe.

The comment at the top of `orders.xml` is gone. EXI keeps what most applications read, the
elements, attributes and text, and leaves out comments, processing
instructions, the document type declaration and namespace prefixes
unless asked to keep them. That choice is one of the stream's options.

## Options

Two programs exchanging EXI agree on its options: what the stream
keeps, how its bits are laid out, and whether it is compressed. The
decoder needs the options the encoder used; a stream can carry them in
its header (`--include-options`), or the two sides agree on them
beforehand, as these examples do by giving both commands the same flags.

What a stream keeps is set with `--preserve`, a list of `comments`,
`pis`, `dtd`, `prefixes` and `lexical-values`. `invoice.xml` has a
comment and a namespace prefix:

```console
$ sxml encode --output=invoice.exi invoice.xml
$ sxml decode invoice.exi
<ns0:invoice xmlns:ns0="urn:example:invoice" number="2026-0412">
  
  <ns0:order ref="A-1007"/>
  <ns0:amount currency="EUR">113.40</ns0:amount>
</ns0:invoice>
$ sxml encode --preserve=comments,prefixes --output=invoice-full.exi invoice.xml
$ sxml decode --preserve=comments,prefixes invoice-full.exi
<inv:invoice xmlns:inv="urn:example:invoice" number="2026-0412">
  <!-- Net 30 days. -->
  <inv:order ref="A-1007"/>
  <inv:amount currency="EUR">113.40</inv:amount>
</inv:invoice>
```

Without `prefixes`, the names keep their namespace and lose their
prefix, so the decoder makes one up (`ns0`). The document means the same
to any namespace-aware reader, and the comment's line is still there,
since the white space around it is text.

How the bits are laid out is the alignment. The default, `bit-packed`,
writes each event in as few bits as it needs. `byte-alignment` starts
every value on a byte, which is larger but easier to look at in a hex
dump. `pre-compression` and `compression` regroup the stream: the
structure first, then the values of each attribute and element
together, because values of one name are alike, and `compression` runs
each group through DEFLATE, the algorithm of gzip:

```console
$ sxml encode --alignment=byte-alignment --output=orders.byte.exi orders.xml
$ sxml encode --alignment=compression --output=orders.deflate.exi orders.xml
$ gzip -9 -c orders.xml > orders.xml.gz
$ sxml xpath '("orders.byte.exi", "orders.deflate.exi", "orders.xml.gz") ! `{.}: {bin:length(unparsed-binary(.))} bytes`'
"orders.byte.exi: 3862 bytes"
"orders.deflate.exi: 1011 bytes"
"orders.xml.gz: 1285 bytes"
```

The compressed stream is smaller than the gzip of the text, and it is
still EXI: a decoder reads its events one after another, without
writing the text first. For a stream that has to be small, use
`compression`; for one that has to be fast to write and read, the
default.

The other options are `--fragment`, for a stream of several elements
with no document around them, `--block-size`, the number of values a
compressed stream groups at a time, and `--value-max-length` and
`--value-partition-capacity`, which bound the table of values the
encoder remembers. Each one changes the stream, so the decoder needs the
same.

## Querying EXI

A command that reads a document reads EXI with `--input-format=exi`,
taking the same option flags:

```console
$ sxml xpath --input-format=exi --context-item=orders.exi 'count(//order), sum(//total)'
40
1.45528e4
$ sxml xpath --input-format=exi --context-item=orders.deflate.exi --alignment=compression \
    '//order[@status = "refunded"]/@id/string()'
"A-1006"
"A-1007"
"A-1008"
"A-1011"
"A-1021"
"A-1027"
"A-1028"
"A-1037"
"A-1038"
```

Inside a query, `exi:serialize` makes the stream of a node, as
`xs:base64Binary`, and `exi:parse` reads one back. Their options are a
map, whose `options` entry is the stream's options written as the EXI
options document of the standard, the `header` element in the namespace
`http://www.w3.org/2009/exi`:

```console
$ sxml xquery --allow-file-access --method=text --expression='
    let $options := <header xmlns="http://www.w3.org/2009/exi">
                      <common><compression/></common>
                    </header>
    let $stream := exi:serialize(doc("orders.xml"), { "options": $options })
    return (bin:length($stream),
            deep-equal(exi:parse($stream, { "options": $options }), doc("orders.xml")))'
1011 true
```

The prefix `exi` is bound to sxml's namespace for EXI, `urn:sxml:exi`,
in the `sxml` command and the JavaScript package; a stylesheet declares
it. A query or a stylesheet can also write its result as EXI with the
output method `exi:exi`:

```console
$ sxml xquery --context-item=orders.xml --output=paid.exi \
    --expression='declare option output:method "exi:exi"; <paid>{//order[@status = "paid"]}</paid>'
$ sxml xpath --input-format=exi --context-item=paid.exi 'count(/paid/order)'
24
```
