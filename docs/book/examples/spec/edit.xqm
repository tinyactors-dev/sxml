(:~ Edits of a draft in the xml2rfc version 3 vocabulary. Each function
    is an updating function that changes only the nodes it names, and
    keeps the draft's line breaks and indentation, so that a diff of the
    draft before and after shows the edit and nothing else. :)
module namespace edit = "urn:example:edit";

import module namespace spec = "urn:example:spec" at "spec.xqm";

(: The whitespace in the constructors below is the draft's indentation. :)
declare boundary-space preserve;

(:~ The keywords of BCP 14. :)
declare type edit:keyword as enum(
  "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
  "SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED",
  "MAY", "OPTIONAL"
);

(:~ An RFC to add to the references. :)
declare record edit:rfc(
  number as xs:integer,
  title as xs:string,
  authors as xs:string+,
  year as xs:integer,
  month as xs:string,
  normative as xs:boolean := false()
);

(:~ The next requirement anchor that the draft has never used. :)
declare function edit:next-anchor($draft as document-node()) as xs:string {
  let $numbers := $draft//@anchor[matches(., "^req-[0-9]+$")] ! xs:integer(substring-after(., "req-"))
  return "req-" || (max(($numbers, 0)) + 1)
};

(:~ The next version of the draft: its name, as both places write it,
    and its date. :)
declare updating function edit:new-version($draft as document-node(), $date as xs:date) {
  let $name := string($draft/rfc/@docName)
  let $version := xs:integer(tokenize($name, "-")[last()]) + 1
  let $new-name := replace($name, "[0-9]+$", format-integer($version, "00"))
  return (
    replace value of node $draft/rfc/@docName with $new-name,
    replace value of node $draft/rfc/front/seriesInfo[@name = "Internet-Draft"]/@value with $new-name,
    replace value of node $draft/rfc/front/date/@year with year-from-date($date),
    replace value of node $draft/rfc/front/date/@month with format-date($date, "[MNn]"),
    replace value of node $draft/rfc/front/date/@day with day-from-date($date)
  )
};

(:~ Makes a paragraph with a keyword a requirement: it gets the next
    anchor. :)
declare updating function edit:give-anchor($paragraph as element(t)) {
  insert node attribute anchor { edit:next-anchor(root($paragraph)) } into $paragraph
};

(:~ Adds a requirement after another one, with the next anchor and on
    lines of its own. :)
declare updating function edit:add-requirement($after as element(t), $requirement as element(t)) {
  insert nodes (
    edit:indentation($after),
    $requirement transform with {
      insert node attribute anchor { edit:next-anchor(root($after)) } into .
    }
  ) after $after
};

(:~ Changes the keyword of a requirement. :)
declare updating function edit:set-keyword($requirement as element(t), $keyword as edit:keyword) {
  replace value of node exactly-one($requirement/bcp14) with $keyword
};

(:~ Renames a term: its definition, its anchor, and every cross
    reference to it with its text. :)
declare updating function edit:rename-term($term as element(dt), $name as xs:string) {
  let $anchor := "term-" || lower-case(replace(normalize-space($name), " ", "-"))
  return (
    replace value of node $term with $name,
    replace value of node $term/@anchor with $anchor,
    for $xref in root($term)//xref[@target = $term/@anchor]
    return (
      replace value of node $xref/@target with $anchor,
      if (exists($xref/node())) then replace value of node $xref with lower-case($name) else ()
    )
  )
};

(:~ Removes an editorial note, and the line break in front of it. :)
declare updating function edit:remove-note($note as element(cref)) {
  delete nodes ($note, $note/preceding-sibling::node()[1][self::text()][normalize-space() = ""])
};

(:~ Adds an RFC to the normative or informative references. :)
declare updating function edit:add-reference($draft as document-node(), $rfc as edit:rfc) {
  let $references := $draft//references[@anchor = (if ($rfc?normative) then "normative" else "informative")]
  let $last := exactly-one($references/reference[last()])
  let $reference :=
        <reference anchor="RFC{ $rfc?number }" target="https://www.rfc-editor.org/info/rfc{ $rfc?number }">
          <front>
            <title>{ $rfc?title }</title>{
              for $author in $rfc?authors
              let $names := tokenize($author)
              return (text { "&#10;            " }, <author fullname="{ $author }" initials="{ head($names) }" surname="{ foot($names) }"/>)
            }
            <date year="{ $rfc?year }" month="{ $rfc?month }"/>
          </front>
          <seriesInfo name="RFC" value="{ $rfc?number }"/>
        </reference>
  return insert nodes (edit:indentation($last), $reference) after $last
};

(:~ The line break and indentation in front of a node. :)
declare %private function edit:indentation($node as node()) as text()? {
  $node/preceding-sibling::node()[1][self::text()][normalize-space() = ""]
};
