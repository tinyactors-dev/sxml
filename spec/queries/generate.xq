xquery version "4.0";

(:~
 : tools/spec/spec generate [--check]: write each document's page, the
 : Markdown file its @file names, from the articles. With CHECK, write
 : nothing and name each page that is not what it would write. Every
 : write of tools/spec/spec runs it afterwards with INCREMENTAL: then only
 : the pages one of whose inputs (local:inputs) is newer are written, and
 : only their facts computed.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $check as xs:string external := "";
declare variable $incremental as xs:string external := "";

declare function local:page($spec as map(*), $document as element(document)) as xs:string {
  "<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit:"
  || " change it with tools/spec/spec (spec/README.md). -->&#10;&#10;"
  || spec:markdown($spec, $document, 1) => replace("\n+$", "") || "&#10;"
};

(: The articles a document's page is made from: the document, the articles
   it includes or collects, and the facts they show. :)
declare function local:articles($spec as map(*), $document as element(document)) as element()* {
  let $articles := ($document, $document//include ! spec:target($spec, .), $document//collect ! spec:collected($spec, .))
  return ($articles, spec:of-kind($spec, "fact")[@id = $articles//value/@of])
};

(: The files a page is made from: those articles' files and the files their facts read. :)
declare function local:inputs($articles as element()*) as xs:string* {
  distinct-values(($articles ! document-uri(root(.)), $articles/self::fact/source/@file ! spec:uri(.))
                  ! substring-after(., "file://"))
};

let $spec := spec:load()
let $documents :=
  for $document in spec:of-kind($spec, "document")
  let $file := spec:uri($document/@file) => substring-after("file://")
  (: Incremental (after a write): only the pages an input is newer than. :)
  where not($incremental) or not(file:exists($file)) or (
    let $written := file:last-modified($file)
    return some $input in local:inputs(local:articles($spec, $document))
           satisfies file:exists($input) and file:last-modified($input) ge $written)
  order by $document/@file
  return $document
(: The facts those pages show, computed once for all of them. :)
let $valued := spec:with-values($spec, $documents ! local:articles($spec, .))
let $pages := for $document in $documents
              return map { "file": string($document/@file),
                           "path": spec:uri($document/@file) => substring-after("file://"),
                           "text": local:page($valued, spec:one($valued, "document:" || $document/@id)) }
return
  if ($check) then
    let $stale := $pages[not(file:exists(?path)) or unparsed-text(spec:uri(?file)) ne ?text]
    return string-join((
      if (empty($stale)) then count($pages) || " pages are up to date"
      else $stale ! ("stale: " || ?file || " (tools/spec/spec generate writes it)")), "&#10;")
  else (
    for $page in $pages return file:write-text($page?path, $page?text),
    count($pages) || " pages written"
  )
