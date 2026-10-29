xquery version "4.0";

(:~
 : tools/spec/spec list [KIND] [--status=WORD] [--recorded-since=DATE]
 : [--no-summary]: one line per article, in identifier order.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $kind as xs:string external := "";
declare variable $status as xs:string external := "";
declare variable $since as xs:string external := "";
declare variable $missing-summary as xs:string external := "";

let $spec := spec:load()
let $articles := spec:of-kind($spec, $kind[.])
  [$status = "" or tokenize(spec:status(.))[1] = $status or spec:status(.) = $status]
  [$since = "" or spec:date(.) ge $since]
  [$missing-summary = "" or (empty(summary) and not(self::term or @status = "unused"))]
return string-join(
  for $article in $articles
  order by local-name($article),
           spec:number($article),
           string($article/@id)
  return spec:line($article),
  "&#10;")
