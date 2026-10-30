xquery version "4.0";

(:~
 : tools/spec/spec generate [--incremental=yes]: write each document's
 : page, the Markdown file its @file names, from the articles. Every write
 : of tools/spec/spec runs it with INCREMENTAL: then only the pages one of
 : whose inputs (spec:page-inputs) is not older are written, and only
 : their facts computed. `check` compares every page (spec:stale-pages).
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $incremental as xs:string external := "";

let $spec := spec:load()
let $documents :=
  for $document in spec:of-kind($spec, "document")
  let $file := spec:uri($document/@file) => substring-after("file://")
  where not($incremental) or not(file:exists($file)) or (
    let $written := file:last-modified($file)
    return some $input in spec:page-inputs(spec:page-articles($spec, $document))
           satisfies file:exists($input) and file:last-modified($input) ge $written)
  order by $document/@file
  return $document
let $pages := spec:pages($spec, $documents)
return (
  for $page in $pages return file:write-text($page?path, $page?text),
  count($pages) || " pages written"
)
