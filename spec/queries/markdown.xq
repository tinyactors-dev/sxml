xquery version "4.0";

(:~
 : tools/spec/spec markdown NAME: NAME as Markdown with every include
 : expanded; for a document, the whole page people read.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $name as xs:string external;

let $loaded := spec:load()
let $spec := spec:with-values($loaded, spec:one($loaded, $name))
let $article := spec:one($spec, $name)
return spec:markdown($spec, $article, if ($article/self::document) then 1 else 2) || "&#10;"
