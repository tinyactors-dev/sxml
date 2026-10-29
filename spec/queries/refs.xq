xquery version "4.0";

(:~
 : tools/spec/spec refs NAME: every place that refers to NAME, with the
 : words around each reference, grouped by the article it is in.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $name as xs:string external;

declare function local:context($reference as element()) as xs:string {
  let $block := $reference/ancestor::*[self::p or self::cell or self::definition or self::title or self::heading][1]
  let $before := string-length(normalize-space(spec:plain(
    $block/node()[. << $reference or . is $reference or exists(.//* intersect $reference)])))
  let $whole := normalize-space(spec:plain($block/node()))
  let $start := max((1, $before - 90))
  return (if ($start > 1) then "..." else "") || substring($whole, $start, 180) ||
         (if ($start + 180 < string-length($whole)) then "..." else "")
};

let $spec := spec:load()
let $article := spec:one($spec, $name)
let $keys := (spec:key($article), $article/self::term/name ! ("term:" || .))
let $places := for $other in $spec?articles/*
               for $reference in spec:references($other)[spec:reference-key(.) = $keys]
               return $reference
return string-join((
  spec:line($article) || ": " || count($places) || " references",
  for $reference in $places
  let $from := $reference/ancestor::*[last()]
  group by $key := spec:key($from)
  return (
    "",
    spec:line($from[1]),
    for $place in $reference
    return if ($place/self::include) then "  (includes it)" else "  " || local:context($place))
), "&#10;")
