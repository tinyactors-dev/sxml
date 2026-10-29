xquery version "4.0";

(:~
 : Do the documents assemble back to their Markdown sources? For the
 : shadow period (spec/README.md): each document article is rendered with
 : its includes and compared, word by word, with the file it came from.
 : Whitespace, line wrapping, backslash escapes, table rules and the
 : numbers of ordered list items are not compared.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $document as xs:string external := "";

declare function local:words($markdown as xs:string) as xs:string* {
  let $lines := tokenize($markdown, "\n")[not(matches(., "^\s*\|(\s*:?-+:?\s*\|)+\s*$|^\s*-{3,}\s*$"))]
  let $text := string-join($lines ! replace(., "^(\s*>)+", ""), " ")
  let $text := replace($text, "\\([^A-Za-z0-9\s])", "$1")
  let $text := replace($text, "\|", " | ")
  let $text := replace($text, "````+", "```")
  let $text := replace($text, "(^|\s)\d+[.)](\s)", "$1N.$2")
  return tokenize(normalize-space($text), " ")
};

declare function local:first-difference($left as xs:string*, $right as xs:string*) as xs:integer? {
  (for $position in 1 to max((count($left), count($right)))
   where $left[$position] != $right[$position] or empty($left[$position]) or empty($right[$position])
   return $position)[1]
};

(: What was written since the import is not in the sources: leave it out. :)
let $spec := spec:load(spec:load()?articles ! document {
  * transform with { delete node (summary, @superseded-by), if (@status = "superseded") then replace value of node @status with "accepted" else () }
})
for $article in spec:of-kind($spec, "document")[$document = "" or @id = $document]
order by $article/@source
let $source := local:words(unparsed-text(spec:uri($article/@source)))
let $assembled := local:words(spec:markdown($spec, $article, 1))
let $difference := local:first-difference($source, $assembled)
return
  if (empty($difference)) then "same: " || $article/@source || " (" || count($source) || " words)&#10;"
  else string-join((
    "differs: " || $article/@source || " at word " || $difference || " of " || count($source),
    "  source:    " || string-join(subsequence($source, $difference - 8, 24), " "),
    "  assembled: " || string-join(subsequence($assembled, $difference - 8, 24), " ")), "&#10;") || "&#10;"
