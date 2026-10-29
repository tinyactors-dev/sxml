xquery version "4.0";

(:~
 : tools/spec/spec check: what is wrong with the spec (spec:check and
 : spec:not-canonical), then one line of counts. tools/spec/spec also
 : validates every file against spec/schema/spec.xsd and checks that the
 : pages are up to date, and exits with status 1 on an error.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

let $spec := spec:load()
let $lines := (spec:not-canonical($spec), spec:check($spec))
let $errors := $lines[starts-with(., "error:")]
let $articles := spec:of-kind($spec, ())
let $summary :=
  count($articles) || " articles (" ||
  string-join(for $article in $articles group by $kind := local-name($article) order by $kind
              return count($article) || " " || $kind, ", ") || "); " ||
  count($errors) || " errors, " || (count($lines) - count($errors)) || " warnings; " ||
  count($articles[not(self::term or self::document or self::log-entry or @status = "unused")][empty(summary)]) || " without a written summary"
return string-join(($lines, $summary), "&#10;")
