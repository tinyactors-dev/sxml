xquery version "4.0";

(:~
 : tools/spec/spec find WORDS...: the articles whose text has all WORDS
 : (XQuery Full Text, with stemming), best first, with their summaries.
 : Titles and summaries weigh more than the rest of the text.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $words as xs:string external;
declare variable $limit as xs:integer external := 20;

let $spec := spec:load()
let $hits :=
  for $article in spec:of-kind($spec, ())[not(self::document)]
  let $head := spec:plain(($article/title, $article/name, $article/summary))
  let $text := spec:plain(($article/body, $article/definition))
  let $in-head := $head contains text { $words } all words using stemming
  where $in-head or ($text contains text { $words } all words using stemming)
  order by $in-head descending,
           count(tokenize(lower-case($text), "\W+")[. = tokenize(lower-case($words), "\W+")]) descending
  return $article
return string-join((
  count($hits) || " articles have " || $words || (if (count($hits) > $limit) then "; the first " || $limit || ":" else ":"),
  for $article in $hits[position() le $limit]
  return ("  " || spec:line($article), "    " || spec:summary($article))
), "&#10;")
