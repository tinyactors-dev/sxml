xquery version "4.0";

(:~
 : tools/spec/spec selftest: the source form reads back to the same nodes.
 : For every article (and every section of every document), the text
 : `tools/spec/spec text` shows is parsed as `amend --body` and `replace`
 : parse it, and must give the nodes it came from. Prints each one that
 : does not, with the first difference.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $limit as xs:integer external := 20;

declare function local:own($node as element()) as element()* {
  local:without-indentation(
    if ($node/body) then $node/body/* else $node/*[not(self::title or self::summary)] ! (if (self::section) then <section id="{ @id }"/> else .))
};

(: The blocks without the whitespace that indents the file between elements. :)
declare function local:without-indentation($blocks as element()*) as element()* {
  $blocks ! (. transform with {
    delete node .//text()[not(normalize-space())][parent::*[self::list or self::item or self::table or self::row or self::quote or self::body]]
  })
};

declare function local:first-difference($left as node()*, $right as node()*) as xs:string {
  let $position := (for $index in 1 to max((count($left), count($right)))
                    where not(deep-equal($left[$index], $right[$index])) return $index)[1]
  let $one := $left[$position]
  let $other := $right[$position]
  return
    if ($one instance of element() and $other instance of element() and node-name($one) = node-name($other)
        and deep-equal($one/@*, $other/@*) and exists($one/node())) then
      local:first-difference($one/node(), $other/node())
    else "&#10;    stored: " || serialize($one) => substring(1, 300) ||
         "&#10;    parsed: " || serialize($other) => substring(1, 300)
};

let $spec := spec:load()
let $nodes := (spec:of-kind($spec, ())[not(self::term)], spec:of-kind($spec, "document")//section)
let $failures :=
  for $node in $nodes
  let $source := spec:source($spec, $node)
  let $parsed := if ($node/body) then spec:prose-body($spec, $source) else spec:prose($spec, $source)
  where not(deep-equal(local:own($node), $parsed))
  return (if ($node/self::section) then spec:key($node/ancestor::document) || "#" || $node/@id else spec:key($node)) ||
         ":" || local:first-difference(local:own($node), $parsed)
let $inline-failures :=
  for $text in spec:of-kind($spec, ())/(title, summary, definition)
  let $parsed := spec:prose-inline($spec, spec:inline($text/node(), false(), "source"))
  where not(deep-equal(<text>{ $text/node() }</text>, <text>{ $parsed }</text>))
  return spec:key($text/..) || " " || local-name($text) || ":" || local:first-difference(<text>{ $text/node() }</text>/node(), <text>{ $parsed }</text>/node())
return string-join((
  count($nodes) || " articles and sections; " || count($failures) || " do not read back",
  count(spec:of-kind($spec, ())/(title, summary, definition)) || " titles, summaries and definitions; " ||
    count($inline-failures) || " do not read back",
  subsequence(($failures, $inline-failures), 1, $limit)), "&#10;")
