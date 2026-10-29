xquery version "4.0";

(:~
 : tools/spec/spec remove NAME: the file of the article NAME, which
 : tools/spec/spec then deletes, or a refusal when anything still refers
 : to it or needs it (spec:removal-refusals). A document's page goes too.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $name as xs:string external;

let $spec := spec:load()
let $article := spec:one($spec, $name)
let $refusals := if ($article/self::section) then "error: a section is part of its document; amend the document"
                 else spec:removal-refusals($spec, $article)
return
  if (exists($refusals)) then error(xs:QName("spec:refused"), "refused: " || string-join($refusals, "&#10;"))
  else string-join((substring-after(document-uri(root($article)), $spec:root), $article/@file), "&#10;")
