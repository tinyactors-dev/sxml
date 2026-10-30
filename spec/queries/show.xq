xquery version "4.0";

(:~
 : tools/spec/spec show NAME [--depth=N]: one article or section, as much
 : of it as DEPTH asks for.
 :   0: what it is, its state, its summary;
 :   1: and what it cites and what cites it (documents: their outline);
 :   2: and its text as Markdown (documents: included articles as lines).
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $name as xs:string external;
declare variable $depth as xs:integer external := 0;

declare function local:outline($spec as map(*), $node as element(), $indent as xs:string) as xs:string* {
  for $child in $node/(section | .//include[not(ancestor::section[. >> $node or . is $node][1] except $node)])
  return
    if ($child/self::section) then (
      let $includes := $child//include
      return $indent || "#" || $child/@id || "  " || spec:title($child) ||
        (if (exists($includes)) then "  (" || count($includes) || " articles)" else ""),
      local:outline($spec, $child, $indent || "  "))
    else ()
};

let $loaded := spec:load()
let $spec := spec:with-values($loaded, spec:one($loaded, $name))
let $article := spec:one($spec, $name)
let $file := substring-after(document-uri(root($article)), $spec:root)
let $date := spec:date($article)
return string-join((
  spec:line($article),
  "  " || string-join(($date ! ("recorded " || .), spec:included-in($spec, $article) ! ("in " || @id), $file), " · "),
  "  " || spec:summary($article),
  if ($depth ge 1) then (
    if ($article/self::document or $article/self::section) then (
      "  outline:", local:outline($spec, $article, "    "))
    else (
      let $cites := spec:cites($spec, $article)
      return ("  cites (" || count($cites) || "):", $cites ! ("    " || spec:line(.))),
      let $cited := spec:cited-by($spec, $article)
      return ("  cited by (" || count($cited) || "):", $cited ! ("    " || spec:line(.)))))
  else (),
  if ($depth ge 2) then ("", spec:markdown($spec, $article, 2, "outline")) else ()
), "&#10;")
