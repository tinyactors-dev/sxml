xquery version "4.0";

(:~
 : tools/spec/spec brief NAME: what an agent needs to start on NAME,
 : usually a packet. Its whole text; then one level down, the summary of
 : each decision, proposal and packet it cites and the definition of each
 : term it uses. `tools/spec/spec show NAME --depth=2` opens any of them.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $name as xs:string external;

let $loaded := spec:load()
let $spec := spec:with-values($loaded, spec:one($loaded, $name))
let $article := spec:one($spec, $name)
let $cites := spec:cites($spec, $article)
let $terms := $cites/self::term
let $others := $cites except $terms
return string-join((
  "# Brief: " || spec:line($article),
  "",
  spec:markdown($spec, $article, 2),
  "",
  "## What it cites (" || count($others) || ")",
  "",
  for $cited in $others
  order by local-name($cited), $cited/@id
  return ("- " || spec:line($cited), "  " || spec:summary($cited)),
  "",
  "## The terms it uses (" || count($terms) || ")",
  "",
  for $term in $terms
  order by $term/@id
  return "- `" || string-join($term/name, "` / `") || "`: " ||
         string-join($term/definition ! normalize-space(spec:plain(node())), " | "),
  "",
  "(A summary starting with ~ is the start of the article's text, not a written summary.)"
), "&#10;")
