(:~ What changed in the requirements between two versions of a draft:
    requirements added, removed, with another keyword, or with other
    text. Requirements are matched by anchor, which never changes. :)
import module namespace spec = "urn:example:spec" at "spec.xqm";

declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $old as xs:string external := "latch-00.xml";
declare variable $new as xs:string external := "latch.xml";
(:~ "text" or "json". :)
declare variable $format as enum("text", "json") external := "text";

declare record local:change(anchor as xs:string, change as xs:string, old as xs:string?, new as xs:string?);

(:~ The text of a requirement with its keyword left out, so that a new
    keyword alone is not also a new text. :)
declare function local:wording($entry as spec:entry) as xs:string {
  replace($entry?text, $entry?keyword, "...")
};

declare variable $old-entries := spec:requirements(doc($old)) ! spec:entry(.);
declare variable $new-entries := spec:requirements(doc($new)) ! spec:entry(.);

declare variable $changes := (
  for $entry in $new-entries
  let $before := $old-entries[?anchor = $entry?anchor]
  return
    if (empty($before)) then
      local:change($entry?anchor, "added", (), $entry?text)
    else (
      if ($before?keyword ne $entry?keyword)
      then local:change($entry?anchor, "keyword", $before?keyword, $entry?keyword) else (),
      if (local:wording($before) ne local:wording($entry))
      then local:change($entry?anchor, "text", $before?text, $entry?text) else ()
    ),
  for $entry in $old-entries[not(?anchor = $new-entries?anchor)]
  return local:change($entry?anchor, "removed", $entry?text, ())
);

if ($format eq "json")
then serialize(array { $changes }, { "method": "json", "indent": true() })
else
  for $change in $changes
  return switch ($change?change)
    case "added" return `{$change?anchor} added: {$change?new}`
    case "removed" return `{$change?anchor} removed: {$change?old}`
    case "keyword" return `{$change?anchor} keyword: {$change?old} -> {$change?new}`
    default return (`{$change?anchor} text: {$change?old}`, `    now: {$change?new}`)
