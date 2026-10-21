(:~ What the specification project knows about a draft in the xml2rfc
    version 3 vocabulary: its requirements, their keywords, and the
    numbers a reader sees. :)
module namespace spec = "urn:example:spec";

(:~ The keywords of BCP 14 (RFC 2119, RFC 8174), strongest first. :)
declare variable $spec:keywords as xs:string+ := (
  "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
  "SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED",
  "MAY", "OPTIONAL"
);

(:~ One line of the requirements index. :)
declare record spec:entry(
  anchor as xs:string,
  number as xs:integer,
  section as xs:string,
  keyword as xs:string,
  text as xs:string
);

(:~ The requirements of a draft: the paragraphs with an anchor and a
    keyword, in document order. :)
declare function spec:requirements($draft as document-node()) as element(t)* {
  $draft//t[@anchor][bcp14]
};

(:~ The keywords of a paragraph, as BCP 14 writes them. :)
declare function spec:keywords($paragraph as element(t)) as xs:string* {
  $paragraph/bcp14 ! normalize-space()
};

(:~ The number of a section as a reader sees it, such as "3.1". :)
declare function spec:section-number($section as element(section)) as xs:string {
  string-join($section/ancestor-or-self::section ! (count(preceding-sibling::section) + 1), ".")
};

(:~ The number of a requirement as a reader sees it: its position among
    the requirements of the draft. :)
declare function spec:requirement-number($requirement as element(t)) as xs:integer {
  count($requirement/preceding::t[@anchor][bcp14]) + 1
};

(:~ Each anchor of a draft and the element that has it. :)
declare function spec:anchor-map($draft as document-node()) as map(xs:string, element()) {
  map:merge($draft//*[@anchor] ! { string(@anchor): . })
};

(:~ The entry of the requirements index for a requirement. :)
declare function spec:entry($requirement as element(t)) as spec:entry {
  let $section := spec:section($requirement)
  return spec:entry(
    anchor := string($requirement/@anchor),
    number := spec:requirement-number($requirement),
    section := spec:section-number($section),
    keyword := string-join(spec:keywords($requirement), " "),
    text := spec:text($requirement)
  )
};

(:~ A line describing a requirement. :)
declare function spec:describe($requirement as element(t)) as xs:string {
  let $entry := spec:entry($requirement)
  return `{$entry?anchor} ({$entry?keyword}, section {$entry?section}): {$entry?text}`
};

(:~ The section a paragraph is in. :)
declare %private function spec:section($paragraph as element(t)) as element(section) {
  $paragraph/ancestor::section[1]
};

(:~ The text of a paragraph on one line, without editorial notes, and
    with the text a renderer gives an empty cross reference. :)
declare %private function spec:text($paragraph as element(t)) as xs:string {
  normalize-space(string-join(
    for $part in $paragraph/node() except $paragraph/cref
    return
      if ($part instance of element(xref) and empty($part/node()))
      then spec:xref-text($part)
      else string($part)
  ))
};

(:~ What an empty cross reference shows: "Section 4" or "[RFC6238]". :)
declare %private function spec:xref-text($xref as element(xref)) as xs:string {
  let $target := root($xref)//*[@anchor = $xref/@target]
  return
    if ($target instance of element(section))
    then "Section " || spec:section-number($target)
    else "[" || $xref/@target || "]"
};
