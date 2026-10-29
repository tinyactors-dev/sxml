xquery version "4.0";

(:~
 : tools/spec/spec generate [--check]: write each document's page, the
 : Markdown file its @file names, from the articles. With CHECK, write
 : nothing and name each page that is not what it would write. Every
 : write of tools/spec/spec runs this afterwards.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $check as xs:string external := "";

declare function local:page($spec as map(*), $document as element(document)) as xs:string {
  "<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit:"
  || " change it with tools/spec/spec (spec/README.md). -->&#10;&#10;"
  || spec:markdown($spec, $document, 1) => replace("\n+$", "") || "&#10;"
};

let $spec := spec:load()
let $pages := for $document in spec:of-kind($spec, "document") order by $document/@file
              return map { "file": string($document/@file), "text": local:page($spec, $document) }
return
  if ($check) then
    let $stale := $pages[not(file:exists(spec:uri(?file) => substring-after("file://")))
                         or unparsed-text(spec:uri(?file)) ne ?text]
    return string-join((
      if (empty($stale)) then count($pages) || " pages are up to date"
      else $stale ! ("stale: " || ?file || " (tools/spec/spec generate writes it)")), "&#10;")
  else (
    for $page in $pages return file:write-text(spec:uri($page?file) => substring-after("file://"), $page?text),
    count($pages) || " pages written"
  )
