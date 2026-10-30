xquery version "4.0";

(:~
 : tools/spec/spec check: what is wrong with the spec (spec:check and
 : spec:not-canonical), failing facts, stale pages, then one line of
 : counts. tools/spec/spec also validates every file against
 : spec/schema/spec.xsd, and exits with status 1 on an error.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

let $spec := spec:load()
(: Every fact, computed once: for the facts' own check and for the pages. :)
let $valued := spec:with-values($spec, $spec?articles)
let $pages := spec:valued-pages($valued, spec:of-kind($valued, "document"))
let $lines := (spec:not-canonical($spec), spec:check($spec), spec:check-facts($valued?values),
               spec:stale-pages($pages) ! ("error: stale page: " || . || " (tools/spec/spec generate writes it)"))
let $errors := $lines[starts-with(., "error:")]
let $articles := spec:of-kind($spec, ())
let $summary :=
  count($articles) || " articles (" ||
  string-join(for $article in $articles group by $kind := local-name($article) order by $kind
              return count($article) || " " || $kind, ", ") || "); " ||
  count($errors) || " errors, " || (count($lines) - count($errors)) || " warnings; " ||
  count($articles[not(self::term or self::document or self::log-entry or self::fact or @status = "unused")][empty(summary)]) || " without a written summary"
return string-join(($lines, $summary), "&#10;")
